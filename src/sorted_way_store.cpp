#include <algorithm>
#include <bitset>
#include <cstring>
#include <iostream>
#include "external/libpopcnt.h"
#include "external/streamvbyte.h"
#include "external/streamvbyte_zigzag.h"
#include "sorted_way_store.h"
#include "node_store.h"

namespace SortedWayStoreTypes {
	const uint16_t GroupSize = 256;
	const uint16_t ChunkSize = 256;
	const size_t LargeWayAlignment = 64;

	// We encode some things in the length of a way's unused upper bits.
	const uint16_t CompressedWay = 1 << 15;
	const uint16_t ClosedWay = 1 << 14;
	const uint16_t UniformUpperBits = 1 << 13;

	struct ThreadStorage {
		ThreadStorage():
			collectingOrphans(true),
			groupStart(-1),
			localWays(nullptr) {}

		bool collectingOrphans;
		uint64_t groupStart;
		std::vector<std::pair<WayID, std::vector<NodeID>>>* localWays;
		std::vector<uint8_t> encodedWay;
	};

	thread_local std::deque<std::pair<const SortedWayStore*, ThreadStorage>> threadStorage;

	ThreadStorage& s(const SortedWayStore* who) {
		for (auto& entry : threadStorage)
			if (entry.first == who)
				return entry.second;

		threadStorage.push_back(std::make_pair(who, ThreadStorage()));

		auto& rv = threadStorage.back();
		return rv.second;
	}

	// C++ doesn't support variable length arrays declared on stack.
	// g++ and clang support it, but msvc doesn't. Rather than pay the
	// cost of a vector for every decode, we use a thread_local with room for at
	// least 2,000 nodes.
	//
	// Note: these are scratch buffers, so they remain as true thread-locals,
	// and aren't part of ThreadStorage.
	thread_local uint64_t highBytes[2000];
	thread_local uint32_t uint32Buffer[2000];
	thread_local int32_t int32Buffer[2000];
	thread_local uint8_t uint8Buffer[8192];
}

using namespace SortedWayStoreTypes;

SortedWayStore::SortedWayStore(bool compressWays, const NodeStore& nodeStore): compressWays(compressWays), nodeStore(nodeStore) {
	reopen();
}

SortedWayStore::~SortedWayStore() {
	for (const auto entry: allocatedMemory)
		void_mmap_allocator::deallocate(entry.first, entry.second);

	s(this) = ThreadStorage();
}

void SortedWayStore::reopen() {
	for (const auto entry: allocatedMemory)
		void_mmap_allocator::deallocate(entry.first, entry.second);
	allocatedMemory.clear();

	totalWays = 0;
	totalNodes = 0;
	totalGroups = 0;
	totalGroupSpace = 0;
	totalChunks = 0;
	orphanage.clear();
	workerBuffers.clear();

	// Each group can store 64K ways. If we allocate 32K slots,
	// we support 2^31 = 2B ways, or about twice the number used
	// by OSM as of December 2023.
	groups.clear();
	groups.resize(32 * 1024);

}

std::vector<LatpLon> SortedWayStore::at(WayID id) const {
	const size_t groupIndex = id / (GroupSize * ChunkSize);
	const size_t chunk = (id % (GroupSize * ChunkSize)) / ChunkSize;
	const uint64_t chunkMaskByte = chunk / 8;
	const uint64_t chunkMaskBit = chunk % 8;

	const uint64_t wayMaskByte = (id % ChunkSize) / 8;
	const uint64_t wayMaskBit = id % 8;

	GroupInfo* groupPtr = groups[groupIndex];

	if (groupPtr == nullptr) {
		throw std::out_of_range("SortedWayStore::at(" + std::to_string(id) + ") uses non-existent group " + std::to_string(groupIndex));
	}

	size_t chunkOffset = 0;
	{
		chunkOffset = popcnt(groupPtr->chunkMask, chunkMaskByte);
		uint8_t maskByte = groupPtr->chunkMask[chunkMaskByte];
		maskByte = maskByte & ((1 << chunkMaskBit) - 1);
		chunkOffset += popcnt(&maskByte, 1);

		if (!(groupPtr->chunkMask[chunkMaskByte] & (1 << chunkMaskBit)))
			throw std::out_of_range("SortedWayStore: way " + std::to_string(id) + " missing, no chunk");
	}

	ChunkInfo* chunkPtr = (ChunkInfo*)((char*)groupPtr + groupPtr->chunkOffsets[chunkOffset]);
	const size_t numWays = popcnt(chunkPtr->smallWayMask, 32) + popcnt(chunkPtr->bigWayMask, 32);

	uint8_t* const endOfWayOffsetPtr = (uint8_t*)(chunkPtr->wayOffsets + numWays);
	EncodedWay* wayPtr = nullptr;

	{
		size_t wayOffset = 0;
		wayOffset = popcnt(chunkPtr->smallWayMask, wayMaskByte);
		uint8_t maskByte = chunkPtr->smallWayMask[wayMaskByte];
		maskByte = maskByte & ((1 << wayMaskBit) - 1);
		wayOffset += popcnt(&maskByte, 1);
		if (chunkPtr->smallWayMask[wayMaskByte] & (1 << wayMaskBit)) {
			wayPtr = (EncodedWay*)(endOfWayOffsetPtr + chunkPtr->wayOffsets[wayOffset]);
		}
	}

	// If we didn't find it in small ways, look in big ways.
	if (wayPtr == nullptr) {
		size_t wayOffset = 0;
		wayOffset += popcnt(chunkPtr->smallWayMask, 32);
		wayOffset += popcnt(chunkPtr->bigWayMask, wayMaskByte);
		uint8_t maskByte = chunkPtr->bigWayMask[wayMaskByte];
		maskByte = maskByte & ((1 << wayMaskBit) - 1);
		wayOffset += popcnt(&maskByte, 1);
		if (!(chunkPtr->bigWayMask[wayMaskByte] & (1 << wayMaskBit)))
			throw std::out_of_range("SortedWayStore: way " + std::to_string(id) + " missing, no way");

		wayPtr = (EncodedWay*)(endOfWayOffsetPtr + chunkPtr->wayOffsets[wayOffset] * LargeWayAlignment);
	}

	std::vector<NodeID> nodes = SortedWayStore::decodeWay(wayPtr->flags, wayPtr->data);
	std::vector<LatpLon> rv;
	for (const NodeID& node : nodes)
		rv.push_back(nodeStore.at(node));
	return rv;
}

void SortedWayStore::insertLatpLons(std::vector<WayStore::ll_element_t> &newWays) {
	throw std::runtime_error("SortedWayStore does not support insertLatpLons");
}

const void SortedWayStore::insertNodes(const std::vector<std::pair<WayID, std::vector<NodeID>>>& newWays) {
	// read_pbf can call with an empty array if the only ways it read were unable to
	// be processed due to missing nodes, so be robust against empty way vector.
	if (newWays.empty())
		return;

	if (s(this).localWays == nullptr) {
		std::lock_guard<std::mutex> lock(orphanageMutex);
		if (workerBuffers.size() == 0)
			workerBuffers.reserve(256);
		else if (workerBuffers.size() == workerBuffers.capacity())
			throw std::runtime_error("SortedWayStore doesn't support more than 256 cores");
		workerBuffers.push_back(std::vector<std::pair<WayID, std::vector<NodeID>>>());
		s(this).localWays = &workerBuffers.back();
	}

	if (s(this).groupStart == -1) {
		// Mark where the first full group starts, so we know when to transition
		// out of collecting orphans.
		s(this).groupStart = newWays[0].first / (GroupSize * ChunkSize) * (GroupSize * ChunkSize);
	}

	int i = 0;
	while (s(this).collectingOrphans && i < newWays.size()) {
		const auto& el = newWays[i];
		if (el.first >= s(this).groupStart + (GroupSize * ChunkSize)) {
			s(this).collectingOrphans = false;
			// Calculate new groupStart, rounding to previous boundary.
			s(this).groupStart = el.first / (GroupSize * ChunkSize) * (GroupSize * ChunkSize);
			collectOrphans(*s(this).localWays);
			s(this).localWays->clear();
		}
		s(this).localWays->push_back(el);
		i++;
	}

	while(i < newWays.size()) {
		const auto& el = newWays[i];

		if (el.first >= s(this).groupStart + (GroupSize * ChunkSize)) {
			publishGroup(*s(this).localWays);
			s(this).localWays->clear();
			s(this).groupStart = el.first / (GroupSize * ChunkSize) * (GroupSize * ChunkSize);
		}

		s(this).localWays->push_back(el);
		i++;
	}
}

void SortedWayStore::clear() {
	// TODO: why does this function exist in addition to reopen?
	reopen();
}

std::size_t SortedWayStore::size() const {
	return totalWays.load();
}

void SortedWayStore::finalize(unsigned int threadNum) {
	for (const auto& buffer: workerBuffers) {
		if (buffer.size() > 0) {
			collectOrphans(buffer);
		}
	}
	workerBuffers.clear();

	// Empty the orphanage into the index.
	std::vector<std::pair<WayID, std::vector<NodeID>>> copy;
	for (const auto& entry: orphanage) {
		for (const auto& orphan: entry.second)
			copy.push_back(orphan);

		// Orphans may come from different workers, and thus be unsorted.
		std::sort(
			copy.begin(),
			copy.end(), 
			[](auto const &a, auto const &b) { return a.first < b.first; }
		);
		publishGroup(copy);
		copy.clear();
	}

	orphanage.clear();

	std::cout << "SortedWayStore: " << totalGroups << " groups, " << totalChunks << " chunks, " << totalWays.load() << " ways, " << totalNodes.load() << " nodes, " << totalGroupSpace.load() << " bytes" << std::endl;
}

void SortedWayStore::batchStart() {
	s(this).collectingOrphans = true;
	s(this).groupStart = -1;
	if (s(this).localWays == nullptr || s(this).localWays->size() == 0)
		return;

	collectOrphans(*s(this).localWays);
	s(this).localWays->clear();
}

void SortedWayStore::collectOrphans(const std::vector<std::pair<WayID, std::vector<NodeID>>>& orphans) {
	std::lock_guard<std::mutex> lock(orphanageMutex);
	size_t groupIndex = orphans[0].first / (GroupSize * ChunkSize);

	std::vector<std::pair<WayID, std::vector<NodeID>>>& vec = orphanage[groupIndex];
	const size_t i = vec.size();

	vec.resize(i + orphans.size());
	std::copy(orphans.begin(), orphans.end(), vec.begin() + i);
}

std::vector<NodeID> SortedWayStore::decodeWay(uint16_t flags, const uint8_t* input) {
	std::vector<NodeID> rv;

	bool isCompressed = flags & CompressedWay;
	bool isClosed = flags & ClosedWay;

	const uint16_t length = flags & 0b0000011111111111;

	if (!(flags & UniformUpperBits)) {
		// The nodes don't all share the same upper int; unpack which
		// bits are set on a per-node basis.
		for (int i = 0; i <= (length - 1) / 2; i++) {
			uint8_t byte = *input;
			for (int j = i * 2; j < std::min<int>(length, i * 2 + 2); j++) {
				uint64_t highByte = 0;
				highByte |= (byte & 0b00001111);
				byte = byte >> 4;
				highBytes[j] = (highByte << 31);
			}
			input++;
		}
	} else {
		uint8_t setBits = *(uint8_t*)input;
		input++;
		uint64_t highByte = setBits;
		highByte = highByte << 31;
		for (int i = 0; i < length; i++)
			highBytes[i] = highByte;
	}

	if (!isCompressed) {
		// Decode the low ints
		uint32_t* lowIntData = (uint32_t*)input;
		for (int i = 0; i < length; i++)
			rv.push_back(highBytes[i] | lowIntData[i]);
	} else {
		input += 2;

		uint32_t firstInt = *(uint32_t*)(input);
		input += 4;
		rv.push_back(highBytes[0] | firstInt);

		streamvbyte_decode(input, uint32Buffer, length - 1);
		zigzag_delta_decode(uint32Buffer, int32Buffer, length - 1, firstInt);
		for (int i = 1; i < length; i++) {
			uint32_t tmp = int32Buffer[i - 1];
			rv.push_back(highBytes[i] | tmp);
		}
	}

	if (isClosed)
		rv.push_back(rv[0]);
	return rv;
};

uint16_t SortedWayStore::encodeWay(const std::vector<NodeID>& way, std::vector<uint8_t>& output, bool compress) {
	if (way.size() == 0)
		throw std::runtime_error("Cannot encode an empty way");

	if (way.size() > 2000)
		throw std::runtime_error("Way had more than 2,000 nodes");

	bool isClosed = way.size() > 1 && way[0] == way[way.size() - 1];
	output.clear();

	// When the way is closed, store that in a single bit and omit
	// the final point.
	const int max = isClosed ? way.size() - 1 : way.size();

	uint16_t rv = max;

	if (compress)
		rv |= CompressedWay;

	if (isClosed)
		rv |= ClosedWay;

	bool pushUpperBits = false;

	// zigzag encoding can only be done on ints, not uints, so we shift
	// 31 bits, not 32.
	uint32_t upperInt = way[0] >> 31;
	for (int i = 1; i < way.size(); i++) {
		if (way[i] >> 31 != upperInt) {
			pushUpperBits = true;
			break;
		}
	}

	if (pushUpperBits) {
		for (int i = 0; i <= (max - 1) / 2; i++) {
			uint8_t byte = 0;

			bool first = true;
			for (int j = std::min(max, i * 2 + 2) - 1; j >= i * 2; j--) {
				if (!first)
					byte = byte << 4;
				first = false;
				uint8_t upper4Bits = way[j] >> 31;
				if (upper4Bits > 15)
					throw std::runtime_error("unexpectedly high node ID: " + std::to_string(way[j]));
				byte |= upper4Bits;
			}

			output.push_back(byte);
		}
	} else {
		if (upperInt > 15)
			throw std::runtime_error("unexpectedly high node ID");

		rv |= UniformUpperBits;
		output.push_back(upperInt);
	}

	// Push the low bytes.
	if (!compress) {
		const size_t oldSize = output.size();
		output.resize(output.size() + max * 4);
		uint32_t* dataStart = (uint32_t*)(output.data() + oldSize);
		for (int i = 0; i < max; i++) {
			uint32_t lowBits = way[i];
			lowBits = lowBits & 0x7FFFFFFF;
			dataStart[i] = lowBits;
		}
	} else {
		for (int i = 0; i < max; i++) {
			uint32_t truncated = way[i];
			truncated = truncated & 0x7FFFFFFF;
			int32Buffer[i] = truncated;
		}

		zigzag_delta_encode(int32Buffer + 1, uint32Buffer, max - 1, int32Buffer[0]);

		size_t compressedSize = streamvbyte_encode(uint32Buffer, max - 1, uint8Buffer);

		const size_t oldSize = output.size();
		output.resize(output.size() + 2 /* compressed size */ + 4 /* first 32-bit value */ + compressedSize);
		*(uint16_t*)(output.data() + oldSize) = compressedSize;
		*(uint32_t*)(output.data() + oldSize + 2) = way[0];
		*(uint32_t*)(output.data() + oldSize + 2) &= 0x7FFFFFFF;

		memcpy(output.data() + oldSize + 2 + 4, uint8Buffer, compressedSize);
	}

	return rv;
}

void populateMask(uint8_t* mask, const std::vector<uint8_t>& ids) {
	// mask should be a 32-byte array of uint8_t
	memset(mask, 0, 32);
	for (const uint8_t id : ids) {
		const uint64_t maskByte = id / 8;
		const uint64_t maskBit = id % 8;

		mask[maskByte] |= 1 << maskBit;
	}
}

void SortedWayStore::publishGroup(const std::vector<std::pair<WayID, std::vector<NodeID>>>& ways) {
	totalWays += ways.size();
	if (ways.size() == 0) {
		throw std::runtime_error("SortedWayStore: group is empty");
	}
	size_t groupIndex = ways[0].first / (GroupSize * ChunkSize);

	if (groupIndex >= groups.size())
		throw std::runtime_error("SortedWayStore: unexpected groupIndex " + std::to_string(groupIndex));

	if (ways.size() > ChunkSize * GroupSize) {
		std::cout << "groupIndex=" << groupIndex << ", first ID=" << ways[0].first << ", ways.size() = " << ways.size() << std::endl;
		throw std::runtime_error("SortedWayStore: group is too big");
	}

	totalGroups++;

	struct ChunkData {
		uint8_t id;
		std::vector<uint8_t> wayIds;
		std::vector<uint16_t> wayFlags;
		std::deque<std::vector<uint8_t>> encodedWays;
	};

	std::deque<ChunkData> chunks;


	ChunkData* lastChunk = nullptr;

	// Encode the ways and group by chunk - don't allocate final memory yet.
	uint32_t seenNodes = 0;
	for (const auto& way : ways) {
		seenNodes += way.second.size();
		const uint8_t currentChunk = (way.first % (GroupSize * ChunkSize)) / ChunkSize;

		if (lastChunk == nullptr || lastChunk->id != currentChunk) {
			totalChunks++;
			chunks.push_back({});
			lastChunk = &chunks.back();
			lastChunk->id = currentChunk;
		}
		const WayID id = way.first;
		lastChunk->wayIds.push_back(id % ChunkSize);

		uint16_t flags = encodeWay(way.second, s(this).encodedWay, compressWays && way.second.size() >= 4);
		lastChunk->wayFlags.push_back(flags);

		std::vector<uint8_t> encoded;
		encoded.resize(s(this).encodedWay.size());
		memcpy(encoded.data(), s(this).encodedWay.data(), s(this).encodedWay.size());

		lastChunk->encodedWays.push_back(std::move(encoded));
	}
	totalNodes += seenNodes;

	// We now have the sizes of everything, so we can generate the final memory layout.

	// 1. compute the memory that is needed
	size_t groupSpace = sizeof(GroupInfo); // every group needs a GroupInfo
	groupSpace += chunks.size() * sizeof(uint32_t); // every chunk needs a 32-bit offset
	groupSpace += chunks.size() * sizeof(ChunkInfo); // every chunk needs a ChunkInfo
	for (const auto& chunk : chunks) {
		groupSpace += chunk.wayIds.size() * sizeof(uint16_t); // every way need a 16-bit offset

		// Ways that are < 256 bytes get stored in the small ways buffer with
		// no wasted space. Ways that are >= 256 bytes are stored in the large ways
		// buffer with some wasted space.

		size_t smallWaySize = 0;
		size_t largeWaySize = 0;
		for (int i = 0; i < chunk.wayIds.size(); i++) {
			size_t waySize = chunk.encodedWays[i].size() + sizeof(EncodedWay);
			if (waySize < 256) {
				smallWaySize += waySize;
			} else {
				largeWaySize += (((waySize - 1) / LargeWayAlignment) + 1) * LargeWayAlignment;
			}
		}

		groupSpace += smallWaySize;

		if (smallWaySize % LargeWayAlignment != 0)
			groupSpace += LargeWayAlignment - (smallWaySize % LargeWayAlignment);
		groupSpace += largeWaySize;
	}
	// During decoding, the library may read up to STREAMVBYTE_PADDING extra
	// bytes -- ensure that won't cause out-of-bounds reads.
	groupSpace += STREAMVBYTE_PADDING;

	totalGroupSpace += groupSpace;

	// 2. allocate and track the memory
	GroupInfo* groupInfo = nullptr;
	{
		groupInfo = (GroupInfo*)void_mmap_allocator::allocate(groupSpace);
		if (groupInfo == nullptr)
			throw std::runtime_error("SortedWayStore: failed to allocate space for group");
		std::lock_guard<std::mutex> lock(orphanageMutex);
		allocatedMemory.push_back(std::make_pair((void*)groupInfo, groupSpace));
	}

	if (groups[groupIndex] != nullptr)
		throw std::runtime_error("SortedNodeStore: group already present");
	groups[groupIndex] = groupInfo;

	// 3. populate the masks and offsets
	std::vector<uint8_t> chunkIds;
	chunkIds.reserve(chunks.size());
	for (const auto& chunk : chunks)
		chunkIds.push_back(chunk.id);
	populateMask(groupInfo->chunkMask, chunkIds);

	ChunkInfo* chunkPtr = (ChunkInfo*)((char*)groupInfo->chunkOffsets + (sizeof(uint32_t) * chunks.size()));

	for (size_t chunkIndex = 0; chunkIndex < chunks.size(); chunkIndex++) {
		groupInfo->chunkOffsets[chunkIndex] = (char*)chunkPtr - (char*)groupInfo;

		// Populate: smallWayMask, bigWayMask, wayOffsets
		std::vector<uint8_t> smallWays;
		std::vector<uint8_t> bigWays;

		const ChunkData& chunk = chunks[chunkIndex];
		const size_t numWays = chunk.wayIds.size();
		for (int i = 0; i < numWays; i++) {
			const size_t waySize = chunk.encodedWays[i].size() + sizeof(EncodedWay);
			if (waySize < 256) {
				smallWays.push_back(chunk.wayIds[i]);
			} else {
				bigWays.push_back(chunk.wayIds[i]);
			}
		}
		populateMask(chunkPtr->smallWayMask, smallWays);
		populateMask(chunkPtr->bigWayMask, bigWays);

		// Publish the small ways
		uint8_t* const endOfWayOffsetPtr = (uint8_t*)(chunkPtr->wayOffsets + numWays);
		uint8_t* wayStartPtr = endOfWayOffsetPtr;
		int offsetIndex = 0;
		for (int i = 0; i < numWays; i++) {
			const size_t waySize = chunk.encodedWays[i].size() + sizeof(EncodedWay);
			if (waySize < 256) {
				chunkPtr->wayOffsets[offsetIndex] = wayStartPtr - endOfWayOffsetPtr;
				EncodedWay* wayPtr = (EncodedWay*)wayStartPtr;
				wayPtr->flags = chunk.wayFlags[i];
				memcpy(wayPtr->data, chunk.encodedWays[i].data(), chunk.encodedWays[i].size());

				wayStartPtr += sizeof(EncodedWay) + chunk.encodedWays[i].size();
				offsetIndex++;
			}
		}

		// Publish the big ways
		// Offset is scaled for big ways, so make sure we're on a multiple of LargeWayAlignment
		if ((wayStartPtr - endOfWayOffsetPtr) % LargeWayAlignment != 0)
			wayStartPtr += LargeWayAlignment - ((wayStartPtr - endOfWayOffsetPtr) % LargeWayAlignment);
		for (int i = 0; i < numWays; i++) {
			const size_t waySize = chunk.encodedWays[i].size() + sizeof(EncodedWay);
			if (waySize >= 256) {
				uint32_t spaceNeeded = (((waySize - 1) / LargeWayAlignment) + 1) * LargeWayAlignment;
				uint32_t offset = wayStartPtr - endOfWayOffsetPtr;
				if (offset % LargeWayAlignment != 0)
					throw std::runtime_error("big way alignment error");

				chunkPtr->wayOffsets[offsetIndex] = offset / LargeWayAlignment;
				EncodedWay* wayPtr = (EncodedWay*)wayStartPtr;
				wayPtr->flags = chunk.wayFlags[i];
				memcpy(wayPtr->data, chunk.encodedWays[i].data(), chunk.encodedWays[i].size());

				wayStartPtr += spaceNeeded;
				offsetIndex++;
			}
		}


		// Update chunkPtr
		chunkPtr = (ChunkInfo*)wayStartPtr;
	}
}
