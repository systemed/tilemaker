#include <protozero/pbf_message.hpp>
#include <iostream>
#include <vector>
#include "pbf_reader.h"
#include "helpers.h"
#include <algorithm>
#include <cstring>
#include <type_traits>
#include <protozero/varint.hpp>

// Packed varint fields are decoded in one pass over the bytes. The values and the exceptions are
// those of protozero's packed iterators, which decode each element with decode_varint and then
// step over it with skip_varint (the latter never throws after a successful decode_varint and
// steps over the same bytes): an element is a varint of at most 10 bytes (of the 10th byte only
// bit 0 counts); 10 or more continuation bytes throw varint_too_long_exception, a varint cut off
// by the end of the field (shorter than 10 bytes) throws end_of_buffer_exception.
// Fast path (little endian): 8 bytes are left in the field and the element ends within them: decoded
// from one 8-byte load without branches on its length, or 8 single-byte elements at once. All else
// (9 and 10 byte varints, errors, the last bytes of the field) goes through protozero::decode_varint.
// The output vector is sized to the field's byte count (an upper bound of the elements) first,
// then shrunk to the elements decoded.
namespace {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define PBF_READER_SWAR 1
#endif

	// Decodes the field [p, end) into o (static_cast<T> of the elements; with Delta, of the running
	// sums of the zigzag-decoded elements, kept in U, wrapping). State in locals, so that it stays
	// in registers.
	template<bool Delta, class T, class U>
	inline T* decodePacked(const char* p, const char* end, T* o, U& acc) {
		U a = acc;
		auto put = [&](uint64_t v) {
			if (Delta) {
				a = static_cast<U>(a + static_cast<U>(static_cast<uint64_t>(protozero::decode_zigzag64(v))));
				*o++ = static_cast<T>(a);
			} else {
				*o++ = static_cast<T>(v);
			}
		};
#ifdef PBF_READER_SWAR
		constexpr uint64_t high = 0x8080808080808080ULL;
		while (end - p >= 8) {
			uint64_t x;
			std::memcpy(&x, p, 8);
			if ((x & high) == 0) {
				// 8 single-byte elements
				for (int i = 0; i < 8; i++)
					put((x >> (8 * i)) & 0xffU);
				p += 8;
				continue;
			}
			const uint64_t term = ~x & high;
			if (term == 0) {
				// 9 or more bytes: protozero
				put(protozero::decode_varint(&p, end));
				continue;
			}
			const unsigned bits = __builtin_ctzll(term) + 1;	// 8 * length
			x &= (~0ULL >> (64 - bits)) & 0x7f7f7f7f7f7f7f7fULL;
			x = (x & 0x007f007f007f007fULL) | ((x & 0x7f007f007f007f00ULL) >> 1);
			x = (x & 0x00003fff00003fffULL) | ((x & 0x3fff00003fff0000ULL) >> 2);
			x = (x & 0x000000000fffffffULL) | ((x & 0x0fffffff00000000ULL) >> 4);
			put(x);
			p += bits >> 3;
		}
#endif
		while (p != end)
			put(protozero::decode_varint(&p, end));
		acc = a;
		return o;
	}

	// Appends the elements as static_cast<T>, like get_packed_uint32 / get_packed_int32 + push_back
	template<class T>
	inline void appendPacked(protozero::data_view view, std::vector<T>& out) {
		const char* p = view.data();
		const char* end = p + view.size();
		const size_t old = out.size();
		out.resize(old + (end - p));	// at most one element per byte
		uint64_t unused = 0;
		out.resize(decodePacked<false>(p, end, out.data() + old, unused) - out.data());
	}

	// Appends the running sums of the zigzag-decoded elements (get_packed_sint64 + `acc += i`;
	// push_back(acc)), with the sum kept in the accumulator's type (wrapping)
	template<class T, class Acc>
	inline void appendDeltas(protozero::data_view view, std::vector<T>& out, Acc& acc) {
		const char* p = view.data();
		const char* end = p + view.size();
		const size_t old = out.size();
		using U = typename std::make_unsigned<Acc>::type;
		U a = static_cast<U>(acc);
		out.resize(old + (end - p));	// at most one element per byte
		out.resize(decodePacked<true>(p, end, out.data() + old, a) - out.data());
		acc = static_cast<Acc>(a);
	}
}

// Where pbf_processor.cpp has higher-level routines that populate our structures,
// pbf_reader.cpp has low-level tools that interact with the protobuf.
//
// The lifetime of an object is only until someone calls a readXyz function at
// the same or higher level.
//   - e.g. readPrimitiveGroup invalidates the result of a prior readPrimitiveGroup call,
//          but not the result of a prior readBlob call
//
// This allows us to re-use buffers to minimize heap churn and allocation cost.
//
// If you want to persist the data beyond that, you must make a copy in memory
// that you own.

PbfReader::BlobHeader PbfReader::PbfReader::readBlobHeader(std::istream& input) {
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
				// std::cout << "BlobHeader: unknown tag: " << std::to_string(static_cast<uint32_t>(message.tag())) << std::endl;
				message.skip();
		}
	}

	if (type.empty())
		throw std::runtime_error("BlobHeader type is missing");

	if (datasize == -1)
		throw std::runtime_error("BlobHeader datasize is missing");

	return { type, datasize };
}

protozero::data_view PbfReader::PbfReader::readBlob(int32_t datasize, std::istream& input) {
	return readBlob(datasize, input, blobStorage2, false);
}

// Like readBlob, but the decompressed data is stored in `output`, which the caller
// owns; with copyRaw, uncompressed blobs are copied there too.
protozero::data_view PbfReader::PbfReader::readBlob(int32_t datasize, std::istream& input, std::string& output, bool copyRaw) {
	blobStorage.resize(datasize);
	input.read(&blobStorage[0], datasize);
	if (input.eof())
		throw std::runtime_error("readBlob: unexpected eof");

	enum class BlobDataType { None, Raw, Zlib };

	int32_t rawSize = -1;
	BlobDataType dataType = BlobDataType::None;
	protozero::data_view view;
	protozero::pbf_message<Schema::Blob> message{&blobStorage[0], blobStorage.size()};
	while (message.next()) {
		switch (message.tag()) {
			case Schema::Blob::optional_int32_raw_size:
				rawSize = message.get_int32();
				break;
			case Schema::Blob::oneof_data_bytes_raw:
				dataType = BlobDataType::Raw;
				view = message.get_view();
				break;
			case Schema::Blob::oneof_data_bytes_zlib_data:
				dataType = BlobDataType::Zlib;
				view = message.get_view();
				break;
			case Schema::Blob::oneof_data_bytes_lzma_data:
			case Schema::Blob::oneof_data_bytes_lz4_data:
			case Schema::Blob::oneof_data_bytes_zstd_data:
				throw std::runtime_error("Blob: unsupported compression tag: " + std::to_string(static_cast<uint32_t>(message.tag())));
			default:
				throw std::runtime_error("Blob: unknown tag: " + std::to_string(static_cast<uint32_t>(message.tag())));
		}
	}

	if (dataType == BlobDataType::None)
		throw std::runtime_error("Blob: missing data");

	if (dataType == BlobDataType::Raw) {
		if (!copyRaw)
			return view;
		output.assign(view.data(), view.size());
		return { output.data(), output.size() };
	}

	if (rawSize != -1)
		output.resize(rawSize);
	else
		output.clear();
	decompress_string(output, view.data(), view.size(), false);
	return { output.data(), output.size() };
}

PbfReader::HeaderBBox PbfReader::PbfReader::readHeaderBBox(protozero::data_view data) {
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
				throw std::runtime_error("HeaderBBox: unknown tag: " + std::to_string(static_cast<uint32_t>(message.tag())));
		}
	}

	return box;
}

PbfReader::HeaderBlock PbfReader::PbfReader::readHeaderBlock(protozero::data_view data) {
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
				//std::cout << "HeaderBlock: unknown tag: " << std::to_string(static_cast<uint32_t>(message.tag())) << std::endl;
				message.skip();
		}
	}

	return block;
}

void PbfReader::PbfReader::readStringTable(protozero::data_view data, std::vector<protozero::data_view>& stringTable) {
	protozero::pbf_message<Schema::StringTable> message{data};
	while (message.next()) {
		switch (message.tag()) {
			case Schema::StringTable::repeated_bytes_s:
				stringTable.push_back(message.get_view());
				break;
			default:
				throw std::runtime_error("StringTable: unknown tag: " + std::to_string(static_cast<uint32_t>(message.tag())));
		}
	}
}

PbfReader::PrimitiveBlock& PbfReader::PbfReader::readPrimitiveBlock(protozero::data_view data) {
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
				pb.internalGroups.push_back(PrimitiveGroup(
					message.get_view(),
					denseNodes,
					way,
					relation
				));
				break;
			}
			default:
				// ignore data for unknown tags to allow for future extensions
				//std::cout << "HeaderBlock: unknown tag: " << std::to_string(static_cast<uint32_t>(message.tag())) << std::endl;
				message.skip();
		}
	}

	pb.groupsImpl = PrimitiveBlock::PrimitiveGroups(pb.internalGroups);

	return pb;
}

void PbfReader::DenseNodes::readDenseNodes(protozero::data_view data) {
	protozero::pbf_message<Schema::DenseNodes> message{data};

	uint64_t id = 0;
	int32_t lon = 0, lat = 0;
	
	while (message.next()) {
		switch (message.tag()) {
			case Schema::DenseNodes::repeated_sint64_id: {
				appendDeltas(message.get_view(), ids, id);
				break;
			} case Schema::DenseNodes::repeated_sint64_lat: {
				appendDeltas(message.get_view(), lats, lat);
				break;
			}
			case Schema::DenseNodes::repeated_sint64_lon: {
				appendDeltas(message.get_view(), lons, lon);
				break;
			}
			case Schema::DenseNodes::repeated_int32_keys_vals: {
				appendPacked(message.get_view(), keyValues);
				break;
			}

			default:
				// ignore data for unknown tags to allow for future extensions
				//std::cout << "HeaderBlock: unknown tag: " << std::to_string(static_cast<uint32_t>(message.tag())) << std::endl;
				message.skip();
		}
	}

	// Per 0 in keyValues (the end of a node's tags) the node's tag range, then (0, 0) up to the
	// number of nodes. Sized first, then filled.
	{
		const size_t kvSize = keyValues.size();
		const int32_t* kv = keyValues.data();
		const size_t old = tagStart.size();
		const size_t ends = std::count(kv, kv + kvSize, 0);
		tagStart.resize(old + ends);
		tagEnd.resize(old + ends);
		int32_t* ts = tagStart.data() + old;
		int32_t* te = tagEnd.data() + old;
		uint32_t prev = 0;
		for (uint32_t cur = 0; cur < kvSize; cur++) {
			if (kv[cur] == 0) {
				*ts++ = prev;
				*te++ = cur;
				prev = cur + 1;
			}
		}
	}

	if (tagStart.size() < ids.size()) {
		tagStart.resize(ids.size());
		tagEnd.resize(ids.size());
	}
}

PbfReader::PrimitiveGroup::PrimitiveGroup(
	protozero::data_view data,
	DenseNodes& denseNodes,
	Way& way,
	Relation& relation
):
	data(data),
	denseNodes(denseNodes),
	internalWays({this, way}),
	internalRelations({this, relation}),
	denseNodesInitialized(false) {
}

int32_t PbfReader::PrimitiveGroup::translateNodeKeyValue(int32_t i) const {
	return denseNodes.keyValues.at(i);
}

protozero::data_view PbfReader::PrimitiveGroup::getDataView() {
	return data;
}

void PbfReader::PrimitiveGroup::ensureData() {
	// Reset our thread locals.
	denseNodes.clear();
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
				denseNodes.readDenseNodes(message.get_view());
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
				throw std::runtime_error("PrimitiveGroup: unknown tag: " + std::to_string(static_cast<uint32_t>(message.tag())));
		}
	}
}

PbfReader::DenseNodes& PbfReader::PrimitiveGroup::nodes() const { return denseNodes; };
PbfReader::PrimitiveBlock::PrimitiveGroups& PbfReader::PrimitiveBlock::groups() { return groupsImpl; };

void PbfReader::DenseNodes::clear() {
	ids.clear();
	lons.clear();
	lats.clear();
	tagStart.clear();
	tagEnd.clear();
	keyValues.clear();
}

bool PbfReader::DenseNodes::empty() {
	return ids.empty();
}

PbfReader::DenseNodes::Iterator PbfReader::DenseNodes::begin() {
	auto it = Iterator {-1, Node{}, *this};
	++it;
	return it;
}

PbfReader::DenseNodes::Iterator PbfReader::DenseNodes::end() {
	return Iterator {static_cast<int32_t>(ids.size()), Node{}, *this};
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
PbfReader::PrimitiveGroup& PbfReader::PrimitiveBlock::PrimitiveGroups::Iterator::operator*() {
	return (*groups)[offset];
}
PbfReader::PrimitiveBlock::PrimitiveGroups::Iterator PbfReader::PrimitiveBlock::PrimitiveGroups::begin() {
	auto it = PrimitiveBlock::PrimitiveGroups::Iterator {-1, *groups };
	++it;
	return it;
}
PbfReader::PrimitiveBlock::PrimitiveGroups::Iterator PbfReader::PrimitiveBlock::PrimitiveGroups::end() {
	return PrimitiveBlock::PrimitiveGroups::Iterator {static_cast<int32_t>(groups->size()), *groups };
}

PbfReader::PrimitiveGroupType PbfReader::PrimitiveGroup::type() const {
	return internalType;
}

void PbfReader::Ways::Iterator::readWay(protozero::data_view data) {
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
				appendPacked(message.get_view(), way.keys);
				break;
			}
			case Schema::Way::repeated_uint32_vals: {
				appendPacked(message.get_view(), way.vals);
				break;
			}
			case Schema::Way::repeated_sint64_refs: {
				appendDeltas(message.get_view(), way.refs, ref);
				break;
			}
			case Schema::Way::repeated_sint64_lats: {
				appendDeltas(message.get_view(), way.lats, lat);
				break;
			}
			case Schema::Way::repeated_sint64_lons: {
				appendDeltas(message.get_view(), way.lons, lon);
				break;
			}

			default:
				// ignore data for unknown tags to allow for future extensions
				//std::cout << "Way: unknown tag: " << std::to_string(static_cast<uint32_t>(message.tag())) << std::endl;
				message.skip();
		}
	}
}

PbfReader::Ways& PbfReader::PrimitiveGroup::ways() const {
	return internalWays;
}
bool PbfReader::Ways::Iterator::operator!=(Ways::Iterator& other) const {
	return offset != other.offset;
}
void PbfReader::Ways::Iterator::operator++() {
	if (message.next()) {
		readWay(message.get_view());
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
		return Ways::Iterator{protozero::pbf_message<Schema::PrimitiveGroup>{nullptr, 0ul}, -1, way};

	protozero::pbf_message<Schema::PrimitiveGroup> message{pg->getDataView()};
	if (message.next()) {
		protozero::pbf_message<Schema::PrimitiveGroup> message{pg->getDataView()};
		auto it = Ways::Iterator{message, -1, way};
		++it;
		return it;
	}

	return Ways::Iterator{message, -1, way};
}
PbfReader::Ways::Iterator PbfReader::Ways::end() {
	return Ways::Iterator{protozero::pbf_message<Schema::PrimitiveGroup>{nullptr, 0ul}, -1, way};
}

void PbfReader::Relations::Iterator::readRelation(protozero::data_view data) {
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
				appendPacked(message.get_view(), relation.keys);
				break;
			}
			case Schema::Relation::repeated_uint32_vals: {
				appendPacked(message.get_view(), relation.vals);
				break;
			}
			case Schema::Relation::repeated_int32_roles_sid: {
				appendPacked(message.get_view(), relation.roles_sid);
				break;
			}
			case Schema::Relation::repeated_sint64_memids: {
				appendDeltas(message.get_view(), relation.memids, memid);
				break;
			}
			case Schema::Relation::repeated_MemberType_types: {
				appendPacked(message.get_view(), relation.types);
				break;
			}

			default:
				// ignore data for unknown tags to allow for future extensions
				//std::cout << "Way: unknown tag: " << std::to_string(static_cast<uint32_t>(message.tag())) << std::endl;
				message.skip();
		}
	}
}

PbfReader::Relations& PbfReader::PrimitiveGroup::relations() const {
	return internalRelations;
}
bool PbfReader::Relations::Iterator::operator!=(Relations::Iterator& other) const {
	return offset != other.offset;
}
void PbfReader::Relations::Iterator::operator++() {
	if (message.next()) {
		readRelation(message.get_view());
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
		return Relations::Iterator{protozero::pbf_message<Schema::PrimitiveGroup>{nullptr, 0ul}, -1, relation};

	protozero::pbf_message<Schema::PrimitiveGroup> message{pg->getDataView()};
	if (message.next()) {
		protozero::pbf_message<Schema::PrimitiveGroup> message{pg->getDataView()};
		auto it = Relations::Iterator{message, -1, relation};
		++it;
		return it;
	}

	return Relations::Iterator{message, -1, relation};
}
PbfReader::Relations::Iterator PbfReader::Relations::end() {
	return Relations::Iterator{protozero::pbf_message<Schema::PrimitiveGroup>{nullptr, 0ul}, -1, relation};
}

PbfReader::HeaderBlock PbfReader::PbfReader::readHeaderFromFile(std::istream& input) {
	BlobHeader bh = readBlobHeader(input);
	protozero::data_view blob = readBlob(bh.datasize, input);
	HeaderBlock header = readHeaderBlock(blob);

	return header;
}
