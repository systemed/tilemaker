#include <algorithm>
#include <atomic>
#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>
#include <bitset>
#include <cstring>
#include <iostream>
#include "external/libpopcnt.h"
#include "external/streamvbyte.h"
#include "external/streamvbyte_zigzag.h"
#include "sorted_way_store.h"
#include <iterator>
#include "node_store.h"

namespace {
#if (defined(__GNUC__) || defined(__clang__)) && defined(__x86_64__) && !defined(__POPCNT__) && \
	defined(HAVE_ASM_POPCNT) && defined(HAVE_CPUID)
	// Without -mpopcnt, __builtin_popcountll is a call into libgcc. Use the
	// POPCNT instruction (libpopcnt's popcnt64) when the CPU has it.
	const bool cpuHasPopcnt = (get_cpuid() & bit_POPCNT) != 0;
	inline uint64_t popcountWord(uint64_t x) { return cpuHasPopcnt ? popcnt64(x) : popcnt64_bitwise(x); }
#elif defined(__GNUC__) || defined(__clang__)
	inline uint64_t popcountWord(uint64_t x) { return __builtin_popcountll(x); }
#else
	inline uint64_t popcountWord(uint64_t x) { return popcnt64_bitwise(x); }
#endif

	// Number of set bits in the first `size` bytes (at most 32) of a mask: the same count as
	// libpopcnt's popcnt(data, size), without its call and dispatch.
	inline uint64_t maskPopcnt(const void* data, uint64_t size) {
		const uint8_t* p = (const uint8_t*)data;
		uint64_t rv = 0;
		for (; size >= 8; size -= 8, p += 8) {
			uint64_t word;
			memcpy(&word, p, 8);
			rv += popcountWord(word);
		}
		if (size > 0) {
			uint64_t word = 0;
			memcpy(&word, p, size);
			rv += popcountWord(word);
		}
		return rv;
	}
}

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
		std::vector<NodeID> decodedWay;
	};

	thread_local std::deque<std::pair<const SortedWayStore*, ThreadStorage>> threadStorage;

	inline ThreadStorage& s(const SortedWayStore* who) {
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
	s(this); // allocate our ThreadStorage before multi-threading
	reopen();
}

SortedWayStore::~SortedWayStore() {
	s(this) = ThreadStorage();
}

void SortedWayStore::reopen() {
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

bool SortedWayStore::contains(size_t shard, WayID id) const {
	const size_t groupIndex = id / (GroupSize * ChunkSize);
	const size_t chunk = (id % (GroupSize * ChunkSize)) / ChunkSize;
	const uint64_t chunkMaskByte = chunk / 8;
	const uint64_t chunkMaskBit = chunk % 8;

	const uint64_t wayMaskByte = (id % ChunkSize) / 8;
	const uint64_t wayMaskBit = id % 8;

	GroupInfo* groupPtr = groups[groupIndex];

	if (groupPtr == nullptr)
		return false;

	size_t chunkOffset = 0;
	{
		chunkOffset = maskPopcnt(groupPtr->chunkMask, chunkMaskByte);
		uint8_t maskByte = groupPtr->chunkMask[chunkMaskByte];
		maskByte = maskByte & ((1 << chunkMaskBit) - 1);
		chunkOffset += maskPopcnt(&maskByte, 1);

		if (!(groupPtr->chunkMask[chunkMaskByte] & (1 << chunkMaskBit)))
			return false;
	}

	ChunkInfo* chunkPtr = (ChunkInfo*)((char*)groupPtr + groupPtr->chunkOffsets[chunkOffset]);

	{
		size_t wayOffset = 0;
		wayOffset = maskPopcnt(chunkPtr->smallWayMask, wayMaskByte);
		uint8_t maskByte = chunkPtr->smallWayMask[wayMaskByte];
		maskByte = maskByte & ((1 << wayMaskBit) - 1);
		wayOffset += maskPopcnt(&maskByte, 1);
		if (chunkPtr->smallWayMask[wayMaskByte] & (1 << wayMaskBit))
			return true;
	}

	size_t wayOffset = 0;
	wayOffset += maskPopcnt(chunkPtr->smallWayMask, 32);
	wayOffset += maskPopcnt(chunkPtr->bigWayMask, wayMaskByte);
	uint8_t maskByte = chunkPtr->bigWayMask[wayMaskByte];
	maskByte = maskByte & ((1 << wayMaskBit) - 1);
	wayOffset += maskPopcnt(&maskByte, 1);
	if (!(chunkPtr->bigWayMask[wayMaskByte] & (1 << wayMaskBit)))
		return false;

	return true;
}

std::vector<LatpLon> SortedWayStore::at(WayID id) const {
	std::vector<LatpLon> rv;
	at(id, rv);
	return rv;
}

void SortedWayStore::at(WayID id, std::vector<LatpLon>& rv) const {
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
		chunkOffset = maskPopcnt(groupPtr->chunkMask, chunkMaskByte);
		uint8_t maskByte = groupPtr->chunkMask[chunkMaskByte];
		maskByte = maskByte & ((1 << chunkMaskBit) - 1);
		chunkOffset += maskPopcnt(&maskByte, 1);

		if (!(groupPtr->chunkMask[chunkMaskByte] & (1 << chunkMaskBit)))
			throw std::out_of_range("SortedWayStore: way " + std::to_string(id) + " missing, no chunk");
	}

	ChunkInfo* chunkPtr = (ChunkInfo*)((char*)groupPtr + groupPtr->chunkOffsets[chunkOffset]);
	const size_t numWays = maskPopcnt(chunkPtr->smallWayMask, 32) + maskPopcnt(chunkPtr->bigWayMask, 32);

	uint8_t* const endOfWayOffsetPtr = (uint8_t*)(chunkPtr->wayOffsets + numWays);
	EncodedWay* wayPtr = nullptr;

	{
		size_t wayOffset = 0;
		wayOffset = maskPopcnt(chunkPtr->smallWayMask, wayMaskByte);
		uint8_t maskByte = chunkPtr->smallWayMask[wayMaskByte];
		maskByte = maskByte & ((1 << wayMaskBit) - 1);
		wayOffset += maskPopcnt(&maskByte, 1);
		if (chunkPtr->smallWayMask[wayMaskByte] & (1 << wayMaskBit)) {
			wayPtr = (EncodedWay*)(endOfWayOffsetPtr + chunkPtr->wayOffsets[wayOffset]);
		}
	}

	// If we didn't find it in small ways, look in big ways.
	if (wayPtr == nullptr) {
		size_t wayOffset = 0;
		wayOffset += maskPopcnt(chunkPtr->smallWayMask, 32);
		wayOffset += maskPopcnt(chunkPtr->bigWayMask, wayMaskByte);
		uint8_t maskByte = chunkPtr->bigWayMask[wayMaskByte];
		maskByte = maskByte & ((1 << wayMaskBit) - 1);
		wayOffset += maskPopcnt(&maskByte, 1);
		if (!(chunkPtr->bigWayMask[wayMaskByte] & (1 << wayMaskBit)))
			throw std::out_of_range("SortedWayStore: way " + std::to_string(id) + " missing, no way");

		wayPtr = (EncodedWay*)(endOfWayOffsetPtr + chunkPtr->wayOffsets[wayOffset] * LargeWayAlignment);
	}

	ThreadStorage& tls = s(this);
	SortedWayStore::decodeWay(wayPtr->flags, wayPtr->data, tls.decodedWay);
	const std::vector<NodeID>& nodes = tls.decodedWay;
	rv.resize(nodes.size());
	nodeStore.at(nodes.data(), nodes.size(), rv.data());
}

void SortedWayStore::insertLatpLons(std::vector<WayStore::ll_element_t> &newWays) {
	throw std::runtime_error("SortedWayStore does not support insertLatpLons");
}

void SortedWayStore::insertNodes(const std::vector<std::pair<WayID, std::vector<NodeID>>>& newWays) {
	// pbf_processor can call with an empty array if the only ways it read were unable to
	// be processed due to missing nodes, so be robust against empty way vector.
	if (newWays.empty())
		return;

	ThreadStorage& tls = s(this);
	if (tls.localWays == nullptr) {
		std::lock_guard<std::mutex> lock(orphanageMutex);
		if (workerBuffers.size() == 0)
			workerBuffers.reserve(256);
		else if (workerBuffers.size() == workerBuffers.capacity())
			throw std::runtime_error("SortedWayStore doesn't support more than 256 cores");
		workerBuffers.push_back(std::vector<std::pair<WayID, std::vector<NodeID>>>());
		tls.localWays = &workerBuffers.back();
	}

	if (tls.groupStart == -1) {
		// Mark where the first full group starts, so we know when to transition
		// out of collecting orphans.
		tls.groupStart = newWays[0].first / (GroupSize * ChunkSize) * (GroupSize * ChunkSize);
	}

	int i = 0;
	while (tls.collectingOrphans && i < newWays.size()) {
		const auto& el = newWays[i];
		if (el.first >= tls.groupStart + (GroupSize * ChunkSize)) {
			tls.collectingOrphans = false;
			// Calculate new groupStart, rounding to previous boundary.
			tls.groupStart = el.first / (GroupSize * ChunkSize) * (GroupSize * ChunkSize);
			collectOrphans(*tls.localWays);
		}
		tls.localWays->push_back(el);
		i++;
	}

	while(i < newWays.size()) {
		const auto& el = newWays[i];

		if (el.first >= tls.groupStart + (GroupSize * ChunkSize)) {
			publishGroup(*tls.localWays);
			tls.localWays->clear();
			tls.groupStart = el.first / (GroupSize * ChunkSize) * (GroupSize * ChunkSize);
		}

		tls.localWays->push_back(el);
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
	for (auto& buffer: workerBuffers) {
		if (buffer.size() > 0) {
			collectOrphans(buffer);
		}
	}
	workerBuffers.clear();

	// Empty the orphanage into the index. The groups are independent and publishGroup is what the
	// workers call during the pass anyway, so publish them from all threads: one after the other,
	// the planet's leftover groups took ~10 s with every other thread idle. Each group's ways are
	// moved together, published and freed by the thread that took it (freeing ~70M node lists on
	// one thread afterwards took seconds on the planet).
	std::vector<std::vector<std::vector<std::pair<WayID, std::vector<NodeID>>>>*> orphanGroups;
	for (auto& entry: orphanage)
		orphanGroups.push_back(&entry.second);
	std::atomic<size_t> nextGroup(0);
	auto publishOrphans = [&]() {
		std::vector<std::pair<WayID, std::vector<NodeID>>> ways;
		for (size_t i = nextGroup++; i < orphanGroups.size(); i = nextGroup++) {
			auto& pieces = *orphanGroups[i];
			size_t total = 0;
			for (const auto& piece : pieces) total += piece.size();
			ways.clear();
			ways.reserve(total);
			for (auto& piece : pieces)
				for (auto& way : piece) ways.emplace_back(way.first, std::move(way.second));
			std::vector<std::vector<std::pair<WayID, std::vector<NodeID>>>>().swap(pieces);
			// Orphans may come from different workers, and thus be unsorted. (Way IDs are unique,
			// so the order doesn't depend on the order of the pieces.)
			std::sort(
				ways.begin(),
				ways.end(),
				[](auto const &a, auto const &b) { return a.first < b.first; }
			);
			publishGroup(ways);
		}
		std::vector<std::pair<WayID, std::vector<NodeID>>>().swap(ways);
	};
	{
		boost::asio::thread_pool pool(std::max<unsigned int>(1, threadNum));
		for (unsigned int t = 0; t < std::max<unsigned int>(1, threadNum); t++)
			boost::asio::post(pool, publishOrphans);
		pool.join();
	}

	orphanage.clear();

	std::cout << "SortedWayStore: " << totalGroups << " groups, " << totalChunks << " chunks, " << totalWays.load() << " ways, " << totalNodes.load() << " nodes, " << totalGroupSpace.load() << " bytes" << std::endl;
}

void SortedWayStore::batchStart() {
	ThreadStorage& tls = s(this);
	tls.collectingOrphans = true;
	tls.groupStart = -1;
	if (tls.localWays == nullptr || tls.localWays->size() == 0)
		return;

	collectOrphans(*tls.localWays);
}

void SortedWayStore::collectOrphans(std::vector<std::pair<WayID, std::vector<NodeID>>>& orphans) {
	size_t groupIndex = orphans[0].first / (GroupSize * ChunkSize);
	// The orphanage gets the ways (moved, not copied) in a vector of their size; the thread's
	// buffer keeps its capacity for the next pieces. Handing over the buffer itself kept its
	// whole capacity (a group's worth of ways) for every piece until the end of the way phase.
	std::vector<std::pair<WayID, std::vector<NodeID>>> piece;
	piece.reserve(orphans.size());
	std::move(orphans.begin(), orphans.end(), std::back_inserter(piece));
	orphans.clear();
	{
		std::lock_guard<std::mutex> lock(orphanageMutex);
		orphanage[groupIndex].push_back(std::move(piece));
	}
}

std::vector<NodeID> SortedWayStore::decodeWay(uint16_t flags, const uint8_t* input) {
	std::vector<NodeID> rv;
	decodeWay(flags, input, rv);
	return rv;
};

namespace {
	// Read a streamvbyte value of (code + 1) bytes at p, never reading at or beyond `end`.
	inline uint32_t svbValue(const uint8_t* p, const uint8_t* end, uint32_t code) {
		static const uint32_t lengthMask[4] = { 0xFFu, 0xFFFFu, 0xFFFFFFu, 0xFFFFFFFFu };
		if (end - p >= 4) {
			uint32_t val;
			memcpy(&val, p, 4);
			return val & lengthMask[code];
		}
		uint32_t val = 0;
		for (uint32_t b = 0; b <= code; b++)
			val |= (uint32_t)p[b] << (8 * b);
		return val;
	}
}

void SortedWayStore::decodeWay(uint16_t flags, const uint8_t* input, std::vector<NodeID>& rv) {
	const uint16_t length = flags & 0b0000011111111111;
	if (length > 0) {
		// Fast path: write the ids straight into rv, decoding the streamvbyte and zigzag delta
		// in one pass (the same values as the general code below computes).
		const bool closed = flags & ClosedWay;
		rv.resize(length + (closed ? 1 : 0));
		NodeID* out = rv.data();
		const uint8_t* highNibbles = nullptr;
		uint64_t uniformHigh = 0;
		if (!(flags & UniformUpperBits)) {
			highNibbles = input;
			input += (length + 1) / 2;
		} else {
			uniformHigh = (uint64_t)*input << 31;
			input++;
		}
		auto high = [&](size_t i) -> uint64_t {
			if (!highNibbles) return uniformHigh;
			return (uint64_t)((highNibbles[i / 2] >> (4 * (i & 1))) & 0b00001111) << 31;
		};
		if (!(flags & CompressedWay)) {
			for (size_t i = 0; i < length; i++) {
				uint32_t low;
				memcpy(&low, input + 4 * i, 4);
				out[i] = high(i) | low;
			}
		} else {
			uint16_t compressedSize;
			memcpy(&compressedSize, input, 2);
			input += 2;
			uint32_t firstInt;
			memcpy(&firstInt, input, 4);
			input += 4;
			out[0] = high(0) | firstInt;
			// length - 1 zigzag deltas, streamvbyte coded: keys, then the data
			const size_t n = length - 1;
			const uint8_t* keys = input;
			const uint8_t* data = input + (n + 3) / 4;
			const uint8_t* end = input + compressedSize;
			uint32_t prev = firstInt;
			for (size_t k = 0; k < n; k++) {
				const uint32_t code = (keys[k >> 2] >> ((k & 3) * 2)) & 3;
				const uint32_t z = svbValue(data, end, code);
				data += code + 1;
				prev += (z >> 1) ^ (0 - (z & 1));
				out[k + 1] = high(k + 1) | prev;
			}
		}
		if (closed)
			out[length] = out[0];
		return;
	}

	rv.clear();

	bool isCompressed = flags & CompressedWay;
	bool isClosed = flags & ClosedWay;

	rv.reserve(length + (isClosed ? 1 : 0));

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
	ThreadStorage& tls = s(this);
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

		uint16_t flags = encodeWay(way.second, tls.encodedWay, compressWays && way.second.size() >= 4);
		lastChunk->wayFlags.push_back(flags);

		std::vector<uint8_t> encoded;
		encoded.resize(tls.encodedWay.size());
		memcpy(encoded.data(), tls.encodedWay.data(), tls.encodedWay.size());

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
