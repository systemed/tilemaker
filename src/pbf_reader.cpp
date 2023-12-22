#include <protozero/pbf_message.hpp>
#include <iostream>
#include <vector>
#include "pbf_reader.h"
#include "helpers.h"

using namespace PbfReader;

// Where read_pbf.cpp has higher-level routines that populate our structures,
// pbf_reader.cpp has low-level tools that interact with the protobuf.
//
// WARNING: PbfReader adopts several constraints to optimize for tilemaker's
// use case.
//
// Objects returned from PbfReader can only be used on the same thread.
// The lifetime of an object is until the earlier of:
// - the thread calls a readXyz function at the same or higher level
//   - e.g. readPrimitiveGroup invalidates the result of a prior readPrimitiveGroup call,
//          but not the result of a prior readBlob call
// - the thread ends
//
// This allows us to re-use buffers to minimize heap churn and allocation cost.
//
// If you want to persist the data beyond that, you must make a copy in memory
// that you own.

namespace PbfReader {
	thread_local std::string blobStorage; // the blob as stored in the PBF
	thread_local std::string blobStorage2; // the blob after decompression, if needed
	thread_local PbfReader::PrimitiveBlock pb;
	thread_local std::vector<uint64_t> denseNodesIds;
	thread_local std::vector<int32_t> denseNodesLons;
	thread_local std::vector<int32_t> denseNodesLats;
	thread_local std::vector<int32_t> denseNodesTagStart;
	thread_local std::vector<int32_t> denseNodesTagEnd;
	thread_local std::vector<int32_t> denseNodesKeyValues;
	thread_local Way way;
	thread_local Relation relation;
	thread_local Nodes nodesImpl;
}

BlobHeader PbfReader::readBlobHeader(std::istream& input) {
	// See https://wiki.openstreetmap.org/wiki/PBF_Format#File_format
	unsigned int size;
	input.read((char*)&size, sizeof(size));
	if (input.eof()) {
		return {"eof", -1};
	}

	endian_swap(size);
	std::vector<char> data;
	data.resize(size);
	input.read(&data[0], size);

	if (input.eof())
		throw std::runtime_error("readBlobHeader: unexpected eof");

	protozero::pbf_message<Schema::BlobHeader> message{&data[0], data.size()};

	std::string type;
	int32_t datasize = -1;

	while (message.next()) {
		switch (message.tag()) {
			case Schema::BlobHeader::required_string_type:
				type = message.get_string();
				break;
			case Schema::BlobHeader::required_int32_datasize:
				datasize = message.get_int32();
				break;
			default:
				// ignore data for unknown tags to allow for future extensions
				std::cout << "BlobHeader: unknown tag: " << std::to_string(static_cast<uint>(message.tag())) << std::endl;
				message.skip();
		}
	}

	if (type.empty())
		throw std::runtime_error("BlobHeader type is missing");

	if (datasize == -1)
		throw std::runtime_error("BlobHeader datasize is missing");

	return { type, datasize };
}

protozero::data_view PbfReader::readBlob(int32_t datasize, std::istream& input) {
	blobStorage.resize(datasize);
	input.read(&blobStorage[0], datasize);
	if (input.eof())
		throw std::runtime_error("readBlob: unexpected eof");

	int32_t rawSize = -1;
	protozero::data_view view;
	protozero::pbf_message<Schema::Blob> message{&blobStorage[0], blobStorage.size()};
	while (message.next()) {
		switch (message.tag()) {
			case Schema::Blob::optional_int32_raw_size:
				rawSize = message.get_int32();
				break;
			case Schema::Blob::oneof_data_bytes_raw:
				view = message.get_view();
				break;
			case Schema::Blob::oneof_data_bytes_zlib_data:
				view = message.get_view();
				break;
			default:
				throw std::runtime_error("Blob: unknown tag: " + std::to_string(static_cast<uint>(message.tag())));
		}
	}

	if (rawSize == -1)
		// Data is not compressed, can return it directly.
		return view;

	blobStorage2.resize(rawSize);
	decompress_string(blobStorage2, view.data(), view.size(), false);
	return { &blobStorage2[0], blobStorage2.size() };
}

HeaderBBox PbfReader::readHeaderBBox(protozero::data_view data) {
	HeaderBBox box{0, 0, 0, 0};

	protozero::pbf_message<Schema::HeaderBBox> message{data};
	while (message.next()) {
		switch (message.tag()) {
			case Schema::HeaderBBox::required_sint64_left:
				box.minLon = message.get_sint64() / 1000000000.0;
				break;
			case Schema::HeaderBBox::required_sint64_right:
				box.maxLon = message.get_sint64() / 1000000000.0;
				break;
			case Schema::HeaderBBox::required_sint64_bottom:
				box.minLat = message.get_sint64() / 1000000000.0;
				break;
			case Schema::HeaderBBox::required_sint64_top:
				box.maxLat = message.get_sint64() / 1000000000.0;
				break;
			default:
				throw std::runtime_error("HeaderBBox: unknown tag: " + std::to_string(static_cast<uint>(message.tag())));
		}
	}

	return box;
}

HeaderBlock PbfReader::readHeaderBlock(protozero::data_view data) {
	HeaderBlock block{false};

	protozero::pbf_message<Schema::HeaderBlock> message{data};
	while (message.next()) {
		switch (message.tag()) {
			case Schema::HeaderBlock::optional_HeaderBBox_bbox:
				block.hasBbox = true;
				block.bbox = PbfReader::readHeaderBBox(message.get_view());
				break;
			case Schema::HeaderBlock::repeated_string_optional_features: {
				const auto feature = message.get_string();
				block.optionalFeatures.insert(feature);
				break;
			}
			default:
				// ignore data for unknown tags to allow for future extensions
				//std::cout << "HeaderBlock: unknown tag: " << std::to_string(static_cast<uint>(message.tag())) << std::endl;
				message.skip();
		}
	}

	return block;
}

void PbfReader::readStringTable(protozero::data_view data, std::vector<protozero::data_view>& stringTable) {
	protozero::pbf_message<Schema::StringTable> message{data};
	while (message.next()) {
		switch (message.tag()) {
			case Schema::StringTable::repeated_bytes_s:
				stringTable.push_back(message.get_view());
				break;
			default:
				throw std::runtime_error("StringTable: unknown tag: " + std::to_string(static_cast<uint>(message.tag())));
		}
	}
}

PbfReader::PrimitiveBlock& PbfReader::readPrimitiveBlock(protozero::data_view data) {
	pb.stringTable.clear();
	pb.internalGroups.clear();

	protozero::pbf_message<Schema::PrimitiveBlock> message{data};
	while (message.next()) {
		switch (message.tag()) {
			case Schema::PrimitiveBlock::required_StringTable_stringtable:
				// Most of our use cases require the string table, so we eagerly
				// initialize it.
				PbfReader::readStringTable(message.get_view(), pb.stringTable);
				break;
			case Schema::PrimitiveBlock::repeated_PrimitiveGroup_primitivegroup: {
				pb.internalGroups.push_back({message.get_view()});
				break;
			}
			default:
				// ignore data for unknown tags to allow for future extensions
				//std::cout << "HeaderBlock: unknown tag: " << std::to_string(static_cast<uint>(message.tag())) << std::endl;
				message.skip();
		}
	}

	pb.groupsImpl = PrimitiveBlock::PrimitiveGroups(pb.internalGroups);

	return pb;
}

void PbfReader::readDenseNodes(protozero::data_view data) {
	protozero::pbf_message<Schema::DenseNodes> message{data};

	uint64_t id = 0;
	int32_t lon = 0, lat = 0;
	
	while (message.next()) {
		switch (message.tag()) {
			case Schema::DenseNodes::repeated_sint64_id: {
				auto pi = message.get_packed_sint64();
				for (auto i : pi) {
					id += i;
					denseNodesIds.push_back(id);
				}
				break;
			} case Schema::DenseNodes::repeated_sint64_lat: {
				auto pi = message.get_packed_sint64();
				for (auto i : pi) {
					lat += i;
					denseNodesLats.push_back(lat);
				}
				break;
			}
			case Schema::DenseNodes::repeated_sint64_lon: {
				auto pi = message.get_packed_sint64();
				for (auto i : pi) {
					lon += i;
					denseNodesLons.push_back(lon);
				}
				break;
			}
			case Schema::DenseNodes::repeated_int32_keys_vals: {
				auto pi = message.get_packed_int32();
				for (auto kv : pi) {
					denseNodesKeyValues.push_back(kv);
				}
				break;
			}

			default:
				// ignore data for unknown tags to allow for future extensions
				//std::cout << "HeaderBlock: unknown tag: " << std::to_string(static_cast<uint>(message.tag())) << std::endl;
				message.skip();
		}
	}

	for (uint32_t cur = 0, prev = 0; cur < denseNodesKeyValues.size(); cur++) {
		if (denseNodesKeyValues[cur] == 0) {
			denseNodesTagStart.push_back(prev);
			denseNodesTagEnd.push_back(cur);
			prev = cur + 1;
		}
	}

	while(denseNodesTagStart.size() < denseNodesIds.size()) {
		denseNodesTagStart.push_back(0);
		denseNodesTagEnd.push_back(0);
	}
}

PbfReader::PrimitiveGroup::PrimitiveGroup(protozero::data_view data): data(data), denseNodesInitialized(false) {
}

int32_t PbfReader::PrimitiveGroup::translateNodeKeyValue(int32_t i) const {
	return denseNodesKeyValues.at(i);
}

protozero::data_view PbfReader::PrimitiveGroup::getDataView() {
	return data;
}

void PbfReader::PrimitiveGroup::ensureData() {
	// Reset our thread locals.
	denseNodesIds.clear();
	denseNodesLons.clear();
	denseNodesLats.clear();
	denseNodesTagStart.clear();
	denseNodesTagEnd.clear();
	denseNodesKeyValues.clear();
	internalWays.pg = this;
	internalRelations.pg = this;

	protozero::pbf_message<Schema::PrimitiveGroup> message{data};
	if (message.next()) {
		switch (message.tag()) {
			case Schema::PrimitiveGroup::repeated_Node_nodes:
				throw std::runtime_error("PrimitiveGroup: non-dense Nodes are not supported");
				break;
			case Schema::PrimitiveGroup::optional_DenseNodes_dense:
				internalType = PrimitiveGroupType::DenseNodes;
				readDenseNodes(message.get_view());
				break;
			case Schema::PrimitiveGroup::repeated_Way_ways:
				internalType = PrimitiveGroupType::Way;
				break;
			case Schema::PrimitiveGroup::repeated_Relation_relations:
				internalType = PrimitiveGroupType::Relation;
				break;
			case Schema::PrimitiveGroup::repeated_ChangeSet_changesets:
				internalType = PrimitiveGroupType::ChangeSet;
				break;
			default:
				throw std::runtime_error("PrimitiveGroup: unknown tag: " + std::to_string(static_cast<uint>(message.tag())));
		}
	}
}

Nodes& PrimitiveGroup::nodes() const { return nodesImpl; };
PrimitiveBlock::PrimitiveGroups& PrimitiveBlock::groups() { return groupsImpl; };

bool PbfReader::Nodes::Iterator::operator!=(Iterator& other) const {
	return offset != other.offset;
}

void PbfReader::Nodes::Iterator::operator++() {
	offset++;

	if (offset < denseNodesIds.size()) {
		node.id = denseNodesIds[offset];
		node.lon = denseNodesLons[offset];
		node.lat = denseNodesLats[offset];
		node.tagStart = denseNodesTagStart[offset];
		node.tagEnd = denseNodesTagEnd[offset];
	}
}

PbfReader::Nodes::Node& PbfReader::Nodes::Iterator::operator*() {
	return node;
}

bool Nodes::empty() {
	return denseNodesIds.empty();
}

PbfReader::Nodes::Iterator Nodes::begin() {
	auto it = Iterator {-1};
	++it;
	return it;
}

PbfReader::Nodes::Iterator Nodes::end() {
	return Iterator {static_cast<int32_t>(denseNodesIds.size())};
}

bool PbfReader::PrimitiveBlock::PrimitiveGroups::Iterator::operator!=(Iterator& other) const {
	return offset != other.offset;
}
void PbfReader::PrimitiveBlock::PrimitiveGroups::Iterator::operator++() {
	offset++;

	if (offset < groups->size()) {
		(*groups)[offset].ensureData();
	}
}
PrimitiveGroup& PbfReader::PrimitiveBlock::PrimitiveGroups::Iterator::operator*() {
	return (*groups)[offset];
}
PbfReader::PrimitiveBlock::PrimitiveGroups::Iterator PbfReader::PrimitiveBlock::PrimitiveGroups::begin() {
	auto it = PbfReader::PrimitiveBlock::PrimitiveGroups::Iterator {-1, *groups };
	++it;
	return it;
}
PbfReader::PrimitiveBlock::PrimitiveGroups::Iterator PbfReader::PrimitiveBlock::PrimitiveGroups::end() {
	return PrimitiveBlock::PrimitiveGroups::Iterator {static_cast<int32_t>(groups->size()), *groups };
}

PbfReader::PrimitiveGroupType PbfReader::PrimitiveGroup::type() const {
	return internalType;
}

void PbfReader::readWay(protozero::data_view data, Way& way) {
	protozero::pbf_message<Schema::Way> message{data};

	way.id = 0;
	way.keys.clear();
	way.vals.clear();
	way.refs.clear();
	way.lats.clear();
	way.lons.clear();

	uint64_t ref = 0;
	uint32_t lat = 0, lon = 0;
	
	while (message.next()) {
		switch (message.tag()) {
			case Schema::Way::required_int64_id:
				way.id = message.get_int64();
				break;
			case Schema::Way::repeated_uint32_keys: {
				auto pi = message.get_packed_uint32();
				for (auto i : pi) {
					way.keys.push_back(i);
				}
				break;
			}
			case Schema::Way::repeated_uint32_vals: {
				auto pi = message.get_packed_uint32();
				for (auto i : pi) {
					way.vals.push_back(i);
				}
				break;
			}
			case Schema::Way::repeated_sint64_refs: {
				auto pi = message.get_packed_sint64();
				for (auto i : pi) {
					ref += i;
					way.refs.push_back(ref);
				}
				break;
			}
			case Schema::Way::repeated_sint64_lats: {
				auto pi = message.get_packed_sint64();
				for (auto i : pi) {
					lat += i;
					way.lats.push_back(lat);
				}
				break;
			}
			case Schema::Way::repeated_sint64_lons: {
				auto pi = message.get_packed_sint64();
				for (auto i : pi) {
					lon += i;
					way.lons.push_back(lon);
				}
				break;
			}

			default:
				// ignore data for unknown tags to allow for future extensions
				//std::cout << "Way: unknown tag: " << std::to_string(static_cast<uint>(message.tag())) << std::endl;
				message.skip();
		}
	}
}

Ways& PbfReader::PrimitiveGroup::ways() const {
	return internalWays;
}
bool PbfReader::Ways::Iterator::operator!=(PbfReader::Ways::Iterator& other) const {
	return offset != other.offset;
}
void PbfReader::Ways::Iterator::operator++() {
	if (message.next()) {
		readWay(message.get_view(), way);
		offset++;
	} else {
		offset = -1;
	}
}
PbfReader::Way& PbfReader::Ways::Iterator::operator*() {
	return way;
}
bool PbfReader::Ways::empty() {
	return pg->type() != PrimitiveGroupType::Way;
}
PbfReader::Ways::Iterator PbfReader::Ways::begin() {
	if (pg->type() != PrimitiveGroupType::Way)
		return Ways::Iterator{protozero::pbf_message<Schema::PrimitiveGroup>{nullptr, 0}, 0};

	protozero::pbf_message<Schema::PrimitiveGroup> message{pg->getDataView()};
	if (message.next()) {
		protozero::pbf_message<Schema::PrimitiveGroup> message{pg->getDataView()};
		auto it = Ways::Iterator{message, -1};
		++it;
		return it;
	}

	return Ways::Iterator{message, -1};
}
PbfReader::Ways::Iterator PbfReader::Ways::end() {
	if (pg->type() != PrimitiveGroupType::Way)
		return Ways::Iterator{protozero::pbf_message<Schema::PrimitiveGroup>{nullptr, 0}, 0};

	return Ways::Iterator{protozero::pbf_message<Schema::PrimitiveGroup>{nullptr, 0}, -1};
}

void PbfReader::readRelation(protozero::data_view data, Relation& relation) {
	protozero::pbf_message<Schema::Relation> message{data};

	relation.id = 0;
	relation.keys.clear();
	relation.vals.clear();
	relation.memids.clear();
	relation.roles_sid.clear();
	relation.types.clear();

	uint64_t memid = 0;
	
	while (message.next()) {
		switch (message.tag()) {
			case Schema::Relation::required_int64_id:
				relation.id = message.get_int64();
				break;
			case Schema::Relation::repeated_uint32_keys: {
				auto pi = message.get_packed_uint32();
				for (auto i : pi) {
					relation.keys.push_back(i);
				}
				break;
			}
			case Schema::Relation::repeated_uint32_vals: {
				auto pi = message.get_packed_uint32();
				for (auto i : pi) {
					relation.vals.push_back(i);
				}
				break;
			}
			case Schema::Relation::repeated_int32_roles_sid: {
				auto pi = message.get_packed_int32();
				for (auto i : pi) {
					relation.roles_sid.push_back(i);
				}
				break;
			}
			case Schema::Relation::repeated_sint64_memids: {
				auto pi = message.get_packed_sint64();
				for (auto i : pi) {
					memid += i;
					relation.memids.push_back(memid);
				}
				break;
			}
			case Schema::Relation::repeated_MemberType_types: {
				auto pi = message.get_packed_int32();
				for (auto i : pi) {
					relation.types.push_back(i);
				}
				break;
			}

			default:
				// ignore data for unknown tags to allow for future extensions
				//std::cout << "Way: unknown tag: " << std::to_string(static_cast<uint>(message.tag())) << std::endl;
				message.skip();
		}
	}
}

Relations& PbfReader::PrimitiveGroup::relations() const {
	return internalRelations;
}
bool PbfReader::Relations::Iterator::operator!=(PbfReader::Relations::Iterator& other) const {
	return offset != other.offset;
}
void PbfReader::Relations::Iterator::operator++() {
	if (message.next()) {
		readRelation(message.get_view(), relation);
		offset++;
	} else {
		offset = -1;
	}
}
PbfReader::Relation& PbfReader::Relations::Iterator::operator*() {
	return relation;
}
bool PbfReader::Relations::empty() {
	return pg->type() != PrimitiveGroupType::Relation;
}
PbfReader::Relations::Iterator PbfReader::Relations::begin() {
	if (pg->type() != PrimitiveGroupType::Relation)
		return Relations::Iterator{protozero::pbf_message<Schema::PrimitiveGroup>{nullptr, 0}, 0};

	protozero::pbf_message<Schema::PrimitiveGroup> message{pg->getDataView()};
	if (message.next()) {
		protozero::pbf_message<Schema::PrimitiveGroup> message{pg->getDataView()};
		auto it = Relations::Iterator{message, -1};
		++it;
		return it;
	}

	return Relations::Iterator{message, -1};
}
PbfReader::Relations::Iterator PbfReader::Relations::end() {
	if (pg->type() != PrimitiveGroupType::Relation)
		return Relations::Iterator{protozero::pbf_message<Schema::PrimitiveGroup>{nullptr, 0}, 0};

	return Relations::Iterator{protozero::pbf_message<Schema::PrimitiveGroup>{nullptr, 0}, -1};
}

HeaderBlock PbfReader::readHeaderFromFile(std::istream& input) {
	PbfReader::BlobHeader bh = PbfReader::readBlobHeader(input);
	protozero::data_view blob = PbfReader::readBlob(bh.datasize, input);
	PbfReader::HeaderBlock header = PbfReader::readHeaderBlock(blob);

	return header;
}

