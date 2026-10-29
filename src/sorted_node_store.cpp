#include <iostream>
#include <algorithm>
#include <cstring>
#include <string>
#include <map>
#include <bitset>
#include <array>
#include "sorted_node_store.h"
#include "external/libpopcnt.h"
#include "external/streamvbyte.h"
#include "external/streamvbyte_zigzag.h"
#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace SortedNodeStoreTypes {
	const uint16_t GroupSize = 256;
	const uint16_t ChunkSize = 256;
	const uint16_t ChunkAlignment = 16;
	const uint32_t ChunkCompressed = 1 << 31;

	// Direct-mapped, per-thread cache of decoded compressed chunks.
	// Decoded values are stored compactly in rank order (the order of the set
	// bits of the chunk's node mask), together with the mask and the rank at
	// the start of each mask word, so a hit is a bit test, a popcount and a load.
	const size_t ChunkCacheBits = 8;
	const size_t ChunkCacheSize = 1 << ChunkCacheBits;
	const uint64_t ChunkCacheEmpty = ~0ull;

	struct CachedChunk {
		uint64_t nodeMask[4];
		uint32_t rankBase[4];
		LatpLon nodes[ChunkSize];
	};

	struct ChunkCache {
		ChunkCache() {
			for (size_t i = 0; i < ChunkCacheSize; i++)
				keys[i] = ChunkCacheEmpty;
		}

		// Keys are kept apart from the (large) entries so that untouched
		// entries are never paged in.
		uint64_t keys[ChunkCacheSize];
		CachedChunk entries[ChunkCacheSize];
	};

	thread_local ChunkCache* chunkCache = nullptr;
	thread_local std::unique_ptr<ChunkCache> chunkCacheOwner;

	ChunkCache& getChunkCache() {
		if (chunkCache == nullptr) {
			chunkCacheOwner.reset(new ChunkCache());
			chunkCache = chunkCacheOwner.get();
		}
		return *chunkCache;
	}

	inline size_t chunkCacheIndex(uint64_t key) {
		return (size_t)((key * 0x9E3779B97F4A7C15ull) >> (64 - ChunkCacheBits));
	}

	std::atomic<uint64_t> nextGeneration(1);

#if (defined(__GNUC__) || defined(__clang__)) && defined(__x86_64__) && !defined(__POPCNT__) && \
	defined(HAVE_ASM_POPCNT) && defined(HAVE_CPUID)
	// Without -mpopcnt, __builtin_popcountll is a call into libgcc. Use the
	// POPCNT instruction (libpopcnt's popcnt64) when the CPU has it.
	const bool cpuHasPopcnt = (get_cpuid() & bit_POPCNT) != 0;
#define SORTED_NODE_STORE_RUNTIME_POPCNT
#endif

	inline uint64_t popcount64(uint64_t x) {
#if defined(SORTED_NODE_STORE_RUNTIME_POPCNT)
		if (cpuHasPopcnt)
			return popcnt64(x);
		return ::popcnt64_bitwise(x);
#elif defined(__GNUC__) || defined(__clang__)
		return __builtin_popcountll(x);
#elif defined(_MSC_VER) && defined(_M_X64)
		return __popcnt64(x);
#else
		x = x - ((x >> 1) & 0x5555555555555555ull);
		x = (x & 0x3333333333333333ull) + ((x >> 2) & 0x3333333333333333ull);
		x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0Full;
		return (x * 0x0101010101010101ull) >> 56;
#endif
	}

	inline unsigned countTrailingZeros64(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
		return __builtin_ctzll(x);
#elif defined(_MSC_VER) && defined(_M_X64)
		unsigned long index;
		_BitScanForward64(&index, x);
		return index;
#else
		return popcount64((x & (0 - x)) - 1);
#endif
	}

	// Load 8 mask bytes as a word whose bit j is bit (j % 8) of byte (j / 8),
	// matching how the masks are addressed bytewise. Compiles to a plain load
	// on little-endian targets.
	inline uint64_t loadMaskWord(const uint8_t* p) {
		return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
			((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
	}

	// Number of set bits in the 256-bit mask below position `bit`.
	inline size_t maskRank(const uint8_t* mask, size_t bit) {
		const size_t word = bit / 64;
		size_t rv = 0;
		for (size_t i = 0; i < word; i++)
			rv += popcount64(loadMaskWord(mask + i * 8));
		rv += popcount64(loadMaskWord(mask + word * 8) & ((1ull << (bit % 64)) - 1));
		return rv;
	}

	inline size_t maskCount(const uint8_t* mask) {
		return popcount64(loadMaskWord(mask)) + popcount64(loadMaskWord(mask + 8)) +
			popcount64(loadMaskWord(mask + 16)) + popcount64(loadMaskWord(mask + 24));
	}

	inline bool maskTest(const uint8_t* mask, size_t bit) {
		return mask[bit / 8] & (1 << (bit % 8));
	}

	// Locate the chunk holding `id`; throws std::out_of_range if its group or
	// chunk is missing.
	inline const ChunkInfoBase* findChunk(GroupInfo* const* groups, NodeID id) {
		const size_t groupIndex = id / (GroupSize * ChunkSize);
		const size_t chunk = (id % (GroupSize * ChunkSize)) / ChunkSize;

		const GroupInfo* groupPtr = groups[groupIndex];

		if (groupPtr == nullptr) {
			throw std::out_of_range("SortedNodeStore::at(" + std::to_string(id) + ") uses non-existent group " + std::to_string(groupIndex));
		}

		if (!maskTest(groupPtr->chunkMask, chunk))
			throw std::out_of_range("SortedNodeStore: node " + std::to_string(id) + " missing, no chunk");
		const size_t chunkOffset = maskRank(groupPtr->chunkMask, chunk);

		uint16_t scaledOffset = groupPtr->chunkOffsets[chunkOffset];
		return (const ChunkInfoBase*)(((const char *)(groupPtr->chunkOffsets + maskCount(groupPtr->chunkMask))) + (scaledOffset * ChunkAlignment));
	}

	inline LatpLon uncompressedNode(const ChunkInfoBase* basePtr, NodeID id) {
		const UncompressedChunkInfo* ptr = (const UncompressedChunkInfo*)basePtr;
		const size_t nodeIndex = id % ChunkSize;
		if (!maskTest(ptr->nodeMask, nodeIndex))
			throw std::out_of_range("SortedNodeStore: node " + std::to_string(id) + " missing, no node");
		const size_t nodeOffset = maskRank(ptr->nodeMask, nodeIndex);

		return ptr->nodes[nodeOffset];
	}

	inline LatpLon cachedNode(const CachedChunk& cached, NodeID id) {
		const size_t nodeIndex = id % ChunkSize;
		const size_t w = nodeIndex / 64;
		const uint64_t bit = 1ull << (nodeIndex % 64);
		const uint64_t word = cached.nodeMask[w];
		if (!(word & bit))
			throw std::out_of_range("SortedNodeStore: node " + std::to_string(id) + " missing, no node");
		return cached.nodes[cached.rankBase[w] + popcount64(word & (bit - 1))];
	}

	// Same as svb_zigzag_decode_32 in streamvbyte_zigzag.c.
	inline int32_t zigzagDecode(uint32_t val) {
		return (val >> 1) ^ (0-(val & 1));
	}

	// prev + delta with wrap-around, as zigzag_delta_decode computes it.
	inline int32_t addDelta(int32_t prev, uint32_t zigzagged) {
		return (int32_t)((uint32_t)prev + (uint32_t)zigzagDecode(zigzagged));
	}

	const uint32_t svbLengthMask[4] = { 0xFFu, 0xFFFFu, 0xFFFFFFu, 0xFFFFFFFFu };

	// Read a streamvbyte value of (code + 1) bytes at p; p + 4 must not be
	// beyond the end of the stream. Assumes little endian, as streamvbyte does.
	inline uint32_t svbLoadUnchecked(const uint8_t* p, uint32_t code) {
		uint32_t val;
		memcpy(&val, p, 4);
		return val & svbLengthMask[code];
	}

	// Read a streamvbyte value of (code + 1) bytes at p, never reading at or
	// beyond `end`.
	inline uint32_t svbLoad(const uint8_t* p, const uint8_t* end, uint32_t code) {
		if (end - p >= 4)
			return svbLoadUnchecked(p, code);
		uint32_t val = 0;
		for (uint32_t b = 0; b <= code; b++)
			val |= (uint32_t)p[b] << (8 * b);
		return val;
	}

	// Decode a compressed chunk into `out`, in rank order.
	void decodeChunk(const CompressedChunkInfo* ptr, CachedChunk& out) {
		const size_t latpSize = (ptr->flags >> 10) & ((1 << 10) - 1);
		const size_t lonSize = ptr->flags & ((1 << 10) - 1);

		uint32_t count = 0;
		for (size_t w = 0; w < 4; w++) {
			const uint64_t word = loadMaskWord(ptr->nodeMask + w * 8);
			out.nodeMask[w] = word;
			out.rankBase[w] = count;
			count += popcount64(word);
		}
		const size_t n = count - 1;

		const uint8_t* latpData = ptr->data;
		const uint8_t* lonData = ptr->data + latpSize;
		int32_t latp = ptr->firstLatp;
		int32_t lon = ptr->firstLon;
		out.nodes[0] = { latp, lon };
		LatpLon* dst = out.nodes + 1;

		if (n < 64) {
			// Fused scalar streamvbyte + zigzag delta decode of both streams.
			const size_t keyLen = (n + 3) / 4;
			const uint8_t* latpEnd = latpData + latpSize;
			const uint8_t* lonEnd = lonData + lonSize;
			const uint8_t* latpPtr = latpData + keyLen;
			const uint8_t* lonPtr = lonData + keyLen;
			// Each value takes at least one byte, so a value followed by at
			// least 3 more lies at least 4 bytes before the end of its stream.
			const size_t unchecked = n > 3 ? n - 3 : 0;
			size_t k = 0;
			for (; k < unchecked; k++) {
				const unsigned shift = (k & 3) * 2;
				const uint32_t latpCode = (latpData[k >> 2] >> shift) & 3;
				const uint32_t lonCode = (lonData[k >> 2] >> shift) & 3;
				latp = addDelta(latp, svbLoadUnchecked(latpPtr, latpCode));
				lon = addDelta(lon, svbLoadUnchecked(lonPtr, lonCode));
				latpPtr += latpCode + 1;
				lonPtr += lonCode + 1;
				dst[k] = { latp, lon };
			}
			for (; k < n; k++) {
				const unsigned shift = (k & 3) * 2;
				const uint32_t latpCode = (latpData[k >> 2] >> shift) & 3;
				const uint32_t lonCode = (lonData[k >> 2] >> shift) & 3;
				latp = addDelta(latp, svbLoad(latpPtr, latpEnd, latpCode));
				lon = addDelta(lon, svbLoad(lonPtr, lonEnd, lonCode));
				latpPtr += latpCode + 1;
				lonPtr += lonCode + 1;
				dst[k] = { latp, lon };
			}
			return;
		}

		uint32_t latpDeltas[ChunkSize];
		uint32_t lonDeltas[ChunkSize];
		streamvbyte_decode(latpData, latpDeltas, n);
		streamvbyte_decode(lonData, lonDeltas, n);
		for (size_t k = 0; k < n; k++) {
			latp = addDelta(latp, latpDeltas[k]);
			lon = addDelta(lon, lonDeltas[k]);
			dst[k] = { latp, lon };
		}
	}

	struct ThreadStorage {
		ThreadStorage():
			collectingOrphans(true),
			groupStart(-1),
			localNodes(nullptr),
			arenaSpace(0),
			arenaPtr(nullptr) {}
		// When SortedNodeStore first starts, it's not confident that it has seen an
		// entire segment, so it's in "collecting orphans" mode. Once it crosses a
		// threshold of 64K elements, it ceases to be in this mode.
		//
		// Orphans are rounded up across multiple threads, and dealt with in
		// the finalize step.
		bool collectingOrphans = true;
		uint64_t groupStart = -1;
		std::vector<NodeStore::element_t>* localNodes = nullptr;

		uint32_t arenaSpace = 0;
		char* arenaPtr = nullptr;
	};

	thread_local std::deque<std::pair<const SortedNodeStore*, ThreadStorage>> threadStorage;

	ThreadStorage& s(const SortedNodeStore* who) {
		for (auto& entry : threadStorage)
			if (entry.first == who)
				return entry.second;

		threadStorage.push_back(std::make_pair(who, ThreadStorage()));

		auto& rv = threadStorage.back();
		return rv.second;
	}
}

using namespace SortedNodeStoreTypes;

SortedNodeStore::SortedNodeStore(bool compressNodes): compressNodes(compressNodes) {
	s(this); // allocate our ThreadStorage before multi-threading
	reopen();
}

void SortedNodeStore::reopen()
{
	allocatedMemory.clear();
	generation = nextGeneration++;

	totalNodes = 0;
	totalGroups = 0;
	totalGroupSpace = 0;
	totalAllocatedSpace = 0;
	totalChunks = 0;
	memset(chunkSizeFreqs, 0, sizeof(chunkSizeFreqs));
	memset(groupSizeFreqs, 0, sizeof(groupSizeFreqs));
	orphanage.clear();
	workerBuffers.clear();

	// Each group can store 64K nodes. If we allocate 256K slots
	// for groups, we support 2^34 = 17B nodes, or about twice
	// the number used by OSM as of November 2023.
	groups.clear();
	groups.resize(256 * 1024);
}

SortedNodeStore::~SortedNodeStore() {
	s(this) = ThreadStorage();
}

bool SortedNodeStore::contains(size_t shard, NodeID id) const {
	const size_t groupIndex = id / (GroupSize * ChunkSize);
	const size_t chunk = (id % (GroupSize * ChunkSize)) / ChunkSize;
	const uint64_t chunkMaskByte = chunk / 8;
	const uint64_t chunkMaskBit = chunk % 8;

	const uint64_t nodeMaskByte = (id % ChunkSize) / 8;
	const uint64_t nodeMaskBit = id % 8;

	GroupInfo* groupPtr = groups[groupIndex];

	if (groupPtr == nullptr)
		return false;

	size_t chunkOffset = 0;
	{
		chunkOffset = popcnt(groupPtr->chunkMask, chunkMaskByte);
		uint8_t maskByte = groupPtr->chunkMask[chunkMaskByte];
		maskByte = maskByte & ((1 << chunkMaskBit) - 1);
		chunkOffset += popcnt(&maskByte, 1);

		if (!(groupPtr->chunkMask[chunkMaskByte] & (1 << chunkMaskBit)))
			return false;
	}

	uint16_t scaledOffset = groupPtr->chunkOffsets[chunkOffset];
	ChunkInfoBase* basePtr = (ChunkInfoBase*)(((char *)(groupPtr->chunkOffsets + popcnt(groupPtr->chunkMask, 32))) + (scaledOffset * ChunkAlignment));

	size_t nodeOffset = 0;
	nodeOffset = popcnt(basePtr->nodeMask, nodeMaskByte);
	uint8_t maskByte = basePtr->nodeMask[nodeMaskByte];
	maskByte = maskByte & ((1 << nodeMaskBit) - 1);
	nodeOffset += popcnt(&maskByte, 1);
	if (!(basePtr->nodeMask[nodeMaskByte] & (1 << nodeMaskBit)))
		return false;


	return true;
}

LatpLon SortedNodeStore::at(const NodeID id) const {
	const uint64_t chunkId = id / ChunkSize;
	if ((chunkId >> 32) != 0) {
		// Not cacheable (chunk ids >= 2^32): decode into scratch space.
		const ChunkInfoBase* basePtr = findChunk(groups.data(), id);
		if (!(basePtr->flags & ChunkCompressed))
			return uncompressedNode(basePtr, id);
		CachedChunk scratch;
		decodeChunk((const CompressedChunkInfo*)basePtr, scratch);
		return cachedNode(scratch, id);
	}

	// Fast path: the chunk was decoded recently on this thread.
	const uint64_t key = (generation << 32) | chunkId;
	ChunkCache& cache = getChunkCache();
	const size_t cacheIndex = chunkCacheIndex(key);
	if (cache.keys[cacheIndex] == key)
		return cachedNode(cache.entries[cacheIndex], id);

	const ChunkInfoBase* basePtr = findChunk(groups.data(), id);
	if (!(basePtr->flags & ChunkCompressed))
		return uncompressedNode(basePtr, id);

	CachedChunk& entry = cache.entries[cacheIndex];
	decodeChunk((const CompressedChunkInfo*)basePtr, entry);
	cache.keys[cacheIndex] = key;
	return cachedNode(entry, id);
}

void SortedNodeStore::at(const NodeID* ids, size_t n, LatpLon* out) const {
	ChunkCache& cache = getChunkCache();
	// Consecutive ids usually share a chunk: reuse its cache entry directly.
	uint64_t lastChunkId = ~0ull;
	const CachedChunk* entry = nullptr;

	for (size_t i = 0; i < n; i++) {
		const NodeID id = ids[i];
		const uint64_t chunkId = id / ChunkSize;
		if (chunkId == lastChunkId) {
			out[i] = cachedNode(*entry, id);
			continue;
		}

		if ((chunkId >> 32) != 0) {
			lastChunkId = ~0ull;
			out[i] = at(id);
			continue;
		}

		const uint64_t key = (generation << 32) | chunkId;
		const size_t cacheIndex = chunkCacheIndex(key);
		if (cache.keys[cacheIndex] != key) {
			const ChunkInfoBase* basePtr = findChunk(groups.data(), id);
			if (!(basePtr->flags & ChunkCompressed)) {
				lastChunkId = ~0ull;
				out[i] = uncompressedNode(basePtr, id);
				continue;
			}
			decodeChunk((const CompressedChunkInfo*)basePtr, cache.entries[cacheIndex]);
			cache.keys[cacheIndex] = key;
		}
		entry = &cache.entries[cacheIndex];
		lastChunkId = chunkId;
		out[i] = cachedNode(*entry, id);
	}

}

size_t SortedNodeStore::size() const {
	// In general, use our atomic counter - it's fastest.
	return totalNodes.load();

	/*
	// This code can be useful when debugging changes to the internal structure.
	size_t rv = 0;
	size_t totalChunks = 0;
	for (const GroupInfo* group: groups) {
		if (group != nullptr) {
			uint64_t chunks = popcnt(group->chunkMask, 32);
			totalChunks += chunks;

			for (size_t i = 0; i < chunks; i++) {
				size_t rawOffset = group->chunkOffsets[i] * ChunkAlignment;
				ChunkInfo* chunk = (ChunkInfo*)(((char*)(&group->chunkOffsets[chunks])) + rawOffset);
				rv += popcnt(chunk->nodeMask, 32);
			}
		}
	}

	std::cout << "SortedNodeStore::size(): totalChunks=" << totalChunks << ", size=" << rv << " (actual nodes: " << totalNodes.load() << ")" << std::endl;
	return rv;
	*/
}

void SortedNodeStore::insert(const std::vector<element_t>& elements) {
	ThreadStorage& tls = s(this);
	if (tls.localNodes == nullptr) {
		std::lock_guard<std::mutex> lock(orphanageMutex);
		if (workerBuffers.size() == 0)
			workerBuffers.reserve(256);
		else if (workerBuffers.size() == workerBuffers.capacity())
			throw std::runtime_error("SortedNodeStore doesn't support more than 256 cores");
		workerBuffers.push_back(std::vector<element_t>());
		tls.localNodes = &workerBuffers.back();
	}

	if (tls.groupStart == -1) {
		// Mark where the first full group starts, so we know when to transition
		// out of collecting orphans.
		tls.groupStart = elements[0].first / (GroupSize * ChunkSize) * (GroupSize * ChunkSize);
	}

	int i = 0;
	while (tls.collectingOrphans && i < elements.size()) {
		const element_t& el = elements[i];
		if (el.first >= tls.groupStart + (GroupSize * ChunkSize)) {
			tls.collectingOrphans = false;
			// Calculate new groupStart, rounding to previous boundary.
			tls.groupStart = el.first / (GroupSize * ChunkSize) * (GroupSize * ChunkSize);
			collectOrphans(*tls.localNodes);
			tls.localNodes->clear();
		}
		tls.localNodes->push_back(el);
		i++;
	}

	while(i < elements.size()) {
		const element_t& el = elements[i];

		if (el.first >= tls.groupStart + (GroupSize * ChunkSize)) {
			publishGroup(*tls.localNodes);
			tls.localNodes->clear();
			tls.groupStart = el.first / (GroupSize * ChunkSize) * (GroupSize * ChunkSize);
		}

		tls.localNodes->push_back(el);
		i++;
	}
}

void SortedNodeStore::batchStart() {
	ThreadStorage& tls = s(this);
	tls.collectingOrphans = true;
	tls.groupStart = -1;
	if (tls.localNodes == nullptr || tls.localNodes->size() == 0)
		return;

	collectOrphans(*tls.localNodes);
	tls.localNodes->clear();
}

void SortedNodeStore::finalize(size_t threadNum) {
	for (const auto& buffer: workerBuffers) {
		if (buffer.size() > 0) {
			collectOrphans(buffer);
		}
	}
	workerBuffers.clear();

	// Empty the orphanage into the index.
	std::vector<element_t> copy;
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

	std::cout << "SortedNodeStore: " << totalGroups << " groups, " << totalChunks << " chunks, " << totalNodes.load() << " nodes, " << totalGroupSpace.load() << " bytes (" << (1000ull * (totalAllocatedSpace.load() - totalGroupSpace.load()) / (totalAllocatedSpace.load() + 1)) / 10.0 << "% wasted)" << std::endl;
	/*
	for (int i = 0; i < 257; i++)
		std::cout << "chunkSizeFreqs[ " << i << " ]= " << chunkSizeFreqs[i].load() << std::endl;
	for (int i = 0; i < 257; i++)
		std::cout << "groupSizeFreqs[ " << i << " ]= " << groupSizeFreqs[i].load() << std::endl;
		*/
}

void SortedNodeStore::collectOrphans(const std::vector<element_t>& orphans) {
	std::lock_guard<std::mutex> lock(orphanageMutex);
	size_t groupIndex = orphans[0].first / (GroupSize * ChunkSize);

	std::vector<element_t>& vec = orphanage[groupIndex];
	const size_t i = vec.size();
	vec.resize(i + orphans.size());
	std::copy(orphans.begin(), orphans.end(), vec.begin() + i);
}

void SortedNodeStore::publishGroup(const std::vector<element_t>& nodes) {
	totalNodes += nodes.size();
	if (nodes.size() == 0) {
		throw std::runtime_error("SortedNodeStore: group is empty");
	}
	size_t groupIndex = nodes[0].first / (GroupSize * ChunkSize);
	if (groupIndex >= groups.size())
		throw std::runtime_error("SortedNodeStore: unexpected groupIndex " + std::to_string(groupIndex));

	if (nodes.size() > ChunkSize * GroupSize) {
		std::cout << "groupIndex=" << groupIndex << ", first ID=" << nodes[0].first << ", nodes.size() = " << nodes.size() << std::endl;
		throw std::runtime_error("SortedNodeStore: group is too big");
	}

	totalGroups++;

	// Calculate the space we need for this group's chunks.

	// Build up the lat/lons for each chunk; we use this to
	// calculate if a compressed version is more efficient.
	int32_t tmpLatpLons[257 * 2] = {0};
	uint32_t tmpLatpLonsZigzag[257 * 2] = {0};
	// NB that we're storing sparse indexes -- so if we had
	// chunks 3, 6 and 7, only the first 3 indexes (0, 1, 2) would be set.
	// compressed[chunkIndex] = 0 => no chunk, else it's the compressed size
	// (or ~0 to skip compression)
	uint32_t compressedLatpSize[256] = {0};
	uint32_t compressedLonSize[256] = {0};
	int64_t lastChunk = -1;
	int64_t currentChunkIndex = 0;
	int64_t currentNodeIndex = 0;
	uint16_t numberNodesInChunk[256] = {0};
	uint8_t compressedBuffer[256 * 4 * 2];

	for (size_t i = 0; i <= nodes.size(); i++) {
		int64_t currentChunk = -1;

		if (i != nodes.size()) {
			const element_t& node = nodes[i];
			currentChunk = (node.first % (GroupSize * ChunkSize)) / ChunkSize;
		}

		if (lastChunk != currentChunk) {
			if (lastChunk != -1) {
				numberNodesInChunk[currentChunkIndex] = currentNodeIndex;
				compressedLatpSize[currentChunkIndex] = ~0;
				compressedLonSize[currentChunkIndex] = ~0;

				if (compressNodes) {
					// Check to see if compression would help.
					// Zigzag-delta-encode the lats/lons, then compress them.
					tmpLatpLonsZigzag[0] = tmpLatpLons[0];
					tmpLatpLonsZigzag[256] = tmpLatpLons[256];
					zigzag_delta_encode(tmpLatpLons + 1, tmpLatpLonsZigzag + 1, currentNodeIndex - 1, tmpLatpLons[0]);
					zigzag_delta_encode(tmpLatpLons + 256 + 1, tmpLatpLonsZigzag + 256 + 1, currentNodeIndex - 1, tmpLatpLons[256]);

					size_t latsCompressedSize = streamvbyte_encode(tmpLatpLonsZigzag + 1, currentNodeIndex - 1, compressedBuffer);
					size_t lonsCompressedSize = streamvbyte_encode(tmpLatpLonsZigzag + 256 + 1, currentNodeIndex - 1, compressedBuffer);

					size_t uncompressedSize = currentNodeIndex * 8;
					size_t totalCompressedSize =
						latsCompressedSize + lonsCompressedSize + // The compressed buffers
						2 * 4; // The initial delta

					// We only allot 10 bits for storing the size of the compressed array--
					// if we need more than 10 bits, we haven't actually been able to
					// compress the array.
					if (totalCompressedSize < uncompressedSize && latsCompressedSize < 1024 && lonsCompressedSize < 1024) {
						compressedLatpSize[currentChunkIndex] = latsCompressedSize;
						compressedLonSize[currentChunkIndex] = lonsCompressedSize;
					}
				}

				currentChunkIndex++;
				currentNodeIndex = 0;
			}

			lastChunk = currentChunk;
		}

		if (i != nodes.size()) {
			tmpLatpLons[currentNodeIndex] = nodes[i].second.latp;
			tmpLatpLons[currentNodeIndex + 256] = nodes[i].second.lon;
			currentNodeIndex++;
		}
	}

	uint64_t chunks = currentChunkIndex;
	totalChunks += chunks;

	size_t groupSpace =
		sizeof(GroupInfo) + // Every group needs a GroupInfo
		chunks * sizeof(uint16_t); // Offsets for each chunk in GroupInfo

	for (currentChunkIndex = 0; currentChunkIndex < 256; currentChunkIndex++) {
		if (compressedLatpSize[currentChunkIndex] == 0)
			break;

		size_t chunkSpace = 0;
		if (compressedLatpSize[currentChunkIndex] == ~0) {
			// Store uncompressed.
			chunkSpace = 
				sizeof(UncompressedChunkInfo) +
				numberNodesInChunk[currentChunkIndex] * sizeof(LatpLon);
		} else {
			chunkSpace = 
				sizeof(CompressedChunkInfo) +
				compressedLatpSize[currentChunkIndex] + compressedLonSize[currentChunkIndex];
		}

		// We require that chunks align on 16-byte boundaries
		if (chunkSpace % ChunkAlignment != 0)
			chunkSpace += ChunkAlignment - (chunkSpace % ChunkAlignment);
		groupSpace += chunkSpace;
	}

	// Per https://github.com/lemire/streamvbyte:
	// During decoding, the library may read up to STREAMVBYTE_PADDING extra
	// bytes from the input buffer (these bytes are read but never used).
	//
	// Thus, we need to reserve at least that much extra to ensure we don't
	// have an out-of-bounds access. We could also allocate from an arena
	// to amortize the cost across many groups, but with 256K groups,
	// the overhead is only 4M, so who cares.
	groupSpace += STREAMVBYTE_PADDING;
	totalGroupSpace += groupSpace;

	GroupInfo* groupInfo = nullptr;

	ThreadStorage& tls = s(this);
	if (tls.arenaSpace < groupSpace) {
		// A full group takes ~330KB. Nodes are read _fast_, and there ends
		// up being contention calling the allocator when reading the
		// planet on a machine with 48 cores -- so allocate in large chunks.
		tls.arenaSpace = 4 * 1024 * 1024;
		totalAllocatedSpace += tls.arenaSpace;
		tls.arenaPtr = (char*)void_mmap_allocator::allocate(tls.arenaSpace);
		if (tls.arenaPtr == nullptr)
			throw std::runtime_error("SortedNodeStore: failed to allocate arena");
		std::lock_guard<std::mutex> lock(orphanageMutex);
		allocatedMemory.push_back(std::make_pair((void*)tls.arenaPtr, tls.arenaSpace));
	}

	tls.arenaSpace -= groupSpace;
	groupInfo = (GroupInfo*)tls.arenaPtr;
	tls.arenaPtr += groupSpace;

	if (groups[groupIndex] != nullptr)
		throw std::runtime_error("SortedNodeStore: group already present");
	groups[groupIndex] = groupInfo;

	lastChunk = -1;
	uint8_t chunkMask[32], nodeMask[32];
	memset(chunkMask, 0, 32);
	memset(nodeMask, 0, 32);

	currentChunkIndex = 0;
	size_t numNodesInChunk = 0;
	size_t chunkNodeStartIndex = 0;

	char* nextChunkInfo = (char*)&(groupInfo->chunkOffsets[chunks]);

	// NB: `i` goes past the end of `nodes` in order that we have
	//  the chance to publish the final ChunkInfo. We take care
	//  not to read past the end of `nodes`, though.
	for (size_t i = 0; i <= nodes.size(); i++) {
		int64_t currentChunk = -1;

		if (i != nodes.size()) {
			const element_t& node = nodes[i];
			currentChunk = (node.first % (GroupSize * ChunkSize)) / ChunkSize;
		}

		if (currentChunk != lastChunk) {
			if (lastChunk != -1) {
				// Publish a ChunkInfo.

				const size_t rawOffset = nextChunkInfo - (char*)(&groupInfo->chunkOffsets[chunks]);
				const size_t scaledOffset = rawOffset / ChunkAlignment;
				if (rawOffset % ChunkAlignment != 0)
					throw std::runtime_error("SortedNodeStore: invalid scaledOffset for chunk");
				if (scaledOffset > 65535)
					throw std::runtime_error("SortedNodeStore: scaledOffset too big (" + std::to_string(scaledOffset) + "), groupIndex=" + std::to_string(groupIndex));

				groupInfo->chunkOffsets[currentChunkIndex] = (uint16_t)(scaledOffset);

				memcpy(((ChunkInfoBase*)nextChunkInfo)->nodeMask, nodeMask, 32);
				if (compressedLatpSize[currentChunkIndex] == ~0) {
					// Store uncompressed.
					((ChunkInfoBase*)nextChunkInfo)->flags = 0;
					for (size_t j = chunkNodeStartIndex; j < i; j++) {
						UncompressedChunkInfo* ptr = (UncompressedChunkInfo*)nextChunkInfo;
						ptr->nodes[j - chunkNodeStartIndex] = nodes[j].second;
					}
				} else {
					// Store compressed.
					CompressedChunkInfo* ptr = (CompressedChunkInfo*)nextChunkInfo;
					ptr->flags = ChunkCompressed | (compressedLatpSize[currentChunkIndex] << 10) | compressedLonSize[currentChunkIndex];

					ptr->firstLatp = nodes[chunkNodeStartIndex].second.latp;
					ptr->firstLon = nodes[chunkNodeStartIndex].second.lon;
					for (size_t j = chunkNodeStartIndex; j < i; j++) {
						tmpLatpLons[j - chunkNodeStartIndex] = nodes[j].second.latp;
						tmpLatpLons[j - chunkNodeStartIndex + 256] = nodes[j].second.lon;
					}

					tmpLatpLonsZigzag[0] = tmpLatpLons[0];
					tmpLatpLonsZigzag[256] = tmpLatpLons[256];
					currentNodeIndex = i - chunkNodeStartIndex;
					zigzag_delta_encode(tmpLatpLons + 1, tmpLatpLonsZigzag + 1, currentNodeIndex - 1, tmpLatpLons[0]);
					zigzag_delta_encode(tmpLatpLons + 256 + 1, tmpLatpLonsZigzag + 256 + 1, currentNodeIndex - 1, tmpLatpLons[256]);

					size_t latsCompressedSize = streamvbyte_encode(tmpLatpLonsZigzag + 1, currentNodeIndex - 1, ptr->data);

					if (latsCompressedSize != compressedLatpSize[currentChunkIndex])
						throw std::runtime_error("unexpected latsCompressedSize");
					size_t lonsCompressedSize = streamvbyte_encode(tmpLatpLonsZigzag + 256 + 1, currentNodeIndex - 1, ptr->data + latsCompressedSize);
					if (lonsCompressedSize != compressedLonSize[currentChunkIndex])
						throw std::runtime_error("unexpected lonsCompressedSize");
				}

				size_t chunkSpace = 0;
				if (compressedLatpSize[currentChunkIndex] == ~0) {
					// Store uncompressed.
					chunkSpace = 
						sizeof(UncompressedChunkInfo) +
						numberNodesInChunk[currentChunkIndex] * sizeof(LatpLon);
				} else {
					chunkSpace = 
						sizeof(CompressedChunkInfo) +
						compressedLatpSize[currentChunkIndex] + compressedLonSize[currentChunkIndex];
				}

				// We require that chunks align on 16-byte boundaries
				if (chunkSpace % ChunkAlignment != 0)
					chunkSpace += ChunkAlignment - (chunkSpace % ChunkAlignment);

				nextChunkInfo += chunkSpace;
				chunkSizeFreqs[numNodesInChunk]++;

				numNodesInChunk = 0;
				memset(nodeMask, 0, 32);

				const uint64_t chunkMaskByte = lastChunk / 8;
				const uint64_t chunkMaskBit = lastChunk % 8;

				chunkMask[chunkMaskByte] |= 1 << chunkMaskBit;
				if (currentChunk != -1)
					currentChunkIndex++;
			}

			lastChunk = currentChunk;
			chunkNodeStartIndex = i;
		}
		numNodesInChunk++;

		if (i != nodes.size()) {
			const element_t& node = nodes[i];

			const uint64_t nodeMaskByte = (node.first % ChunkSize) / 8;
			const uint64_t nodeMaskBit = node.first % 8;
			nodeMask[nodeMaskByte] |= 1 << nodeMaskBit;
		}
	}

	groupSizeFreqs[currentChunkIndex]++;
	memcpy(groupInfo->chunkMask, chunkMask, 32);

	/*
	// debug: verify that we can read every node we just wrote
	for (const auto& node: nodes) {
		const auto rv = at(node.first);

		if (rv.latp != node.second.latp || rv.lon != node.second.lon)
			throw std::runtime_error("failed to roundtrip node ID " + std::to_string(node.first));
	}
	*/
}
