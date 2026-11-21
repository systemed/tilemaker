#include <string>
#include <vector>
#include <iostream>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <random>
#include "tile_data_base.h"
#include "append_vector.h"
#include <boost/sort/sort.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/post.hpp>
#include <functional>
#include <memory>
#include <thread>
#include <condition_variable>
#include <new>
#include <type_traits>

static_assert(sizeof(Z6Offset) <= 2, "interleaveKey assumes Z6Offset has at most 16 bits");
static const unsigned Z6OffsetBits = 8 * sizeof(Z6Offset);

// Spread the low 16 bits of v so that bit i moves to bit 2*i.
static inline uint32_t spreadBits16(uint32_t v) {
	v &= 0x0000FFFF;
	v = (v | (v << 8)) & 0x00FF00FF;
	v = (v | (v << 4)) & 0x0F0F0F0F;
	v = (v | (v << 2)) & 0x33333333;
	v = (v | (v << 1)) & 0x55555555;
	return v;
}

// Bit-interleave the low B bits of x and y, MSB first, with the x bit above
// the y bit at each level. Bits of x and y at or above B are kept as a
// prefix (all of x's high bits, then all of y's high bits), so the key has
// 2 * Z6OffsetBits bits. Requires x, y < 2^Z6OffsetBits and B <= Z6OffsetBits.
static inline uint32_t interleaveKey(uint32_t x, uint32_t y, unsigned B) {
	const uint32_t lowMask = (1u << B) - 1;
	const unsigned highBits = Z6OffsetBits - B;
	const uint32_t low = (spreadBits16(x & lowMask) << 1) | spreadBits16(y & lowMask);
	return (x >> B) << (highBits + 2 * B) |
		(y >> B) << (2 * B) |
		low;
}

// The original ordering: compare x then y at each zoom from CLUSTER_ZOOM to indexZoom.
static inline bool zoomLoopLess(size_t aX, size_t aY, size_t bX, size_t bY, unsigned int indexZoom) {
	for (size_t z = CLUSTER_ZOOM; z <= indexZoom; z++) {
		const auto aXz = aX / (1 << (indexZoom - z));
		const auto bXz = bX / (1 << (indexZoom - z));
		if (aXz != bXz)
			return aXz < bXz;

		const auto aYz = aY / (1 << (indexZoom - z));
		const auto bYz = bY / (1 << (indexZoom - z));

		if (aYz != bYz)
			return aYz < bYz;
	}

	return false;
}

#ifndef NDEBUG
// Debug-only: verify that comparing interleaved keys matches the zoom-loop comparator.
static void checkInterleaveKey() {
	static std::once_flag once;
	std::call_once(once, []() {
		std::mt19937 rng(12345);
		const uint32_t maxVal = (1u << Z6OffsetBits) - 1;
		std::uniform_int_distribution<uint32_t> dist(0, maxVal);
		for (unsigned indexZoom = CLUSTER_ZOOM; indexZoom <= CLUSTER_ZOOM + Z6OffsetBits; indexZoom++) {
			const unsigned B = indexZoom - CLUSTER_ZOOM;
			for (int i = 0; i < 200000; i++) {
				uint32_t aX = dist(rng), aY = dist(rng), bX = dist(rng), bY = dist(rng);
				// Also exercise near-equal pairs
				if (i & 1) { bX = aX ^ (1u << (rng() % Z6OffsetBits)); bY = (i & 2) ? aY : bY; }
				if ((i % 7) == 0) { bX = aX; bY = aY ^ (1u << (rng() % Z6OffsetBits)); }
				bX &= maxVal; bY &= maxVal;
				const uint32_t ka = interleaveKey(aX, aY, B), kb = interleaveKey(bX, bY, B);
				if (zoomLoopLess(aX, aY, bX, bY, indexZoom) != (ka < kb) ||
					zoomLoopLess(bX, bY, aX, aY, indexZoom) != (kb < ka)) {
					std::cerr << "interleaveKey mismatch at indexZoom " << indexZoom << std::endl;
					assert(false);
				}
			}
		}
	});
}
#endif

// The counting sort's buffer, kept by each thread for its next sort (see releaseSortBuffers)
static thread_local std::unique_ptr<uint64_t[]> sortBuffer;
static thread_local size_t sortBufferWords = 0;
static thread_local std::vector<size_t> sortOffsets;

void releaseSortBuffers() {
	sortBuffer.reset();
	sortBufferWords = 0;
	std::vector<size_t>().swap(sortOffsets);
}

namespace {
// Threads 0..n-1 wait in arrive() until all n have arrived
class SortBarrier {
public:
	explicit SortBarrier(size_t n) : n(n) { }
	void arrive() {
		std::unique_lock<std::mutex> lock(m);
		const size_t gen = generation;
		if (++waiting == n) {
			waiting = 0;
			generation++;
			cv.notify_all();
		} else {
			cv.wait(lock, [&]() { return generation != gen; });
		}
	}
private:
	const size_t n;
	size_t waiting = 0, generation = 0;
	std::mutex m;
	std::condition_variable cv;
};
}

// The interleaved key has 2 * Z6OffsetBits bits; it is sorted by a single
// counting-sort pass over all possible key values.
static const size_t keyValues = size_t(1) << (2 * Z6OffsetBits);
// Below this, a comparison sort on the key is cheaper than counting.
static const size_t countingSortMinSize = 65536;

template<typename OO> void sortOutputObjects(
    const unsigned int indexZoom,
    const size_t threadNum,
	typename AppendVectorNS::AppendVector<OO>::Iterator begin,
	typename AppendVectorNS::AppendVector<OO>::Iterator end,
	std::vector<OO>* lowZoom
)
{
    auto collectLowZoom = [&]() {
        if (lowZoom)
            for (auto it = begin; it != end; ++it)
                if (it->oo.minZoom < CLUSTER_ZOOM) lowZoom->push_back(*it);
    };
    // If the user is doing a a small extract, there are few populated
    // entries in `object`.
    //
    // e.g. Colorado has ~9 z6 tiles, 1 of which has 95% of its output
    // objects.
    //
    // This optimizes for the small extract case by doing:
    // - for each vector in objects
    //   - do a multi-threaded sort of vector
    //
    // For small extracts, this ensures that all threads are used even if
    // only a handful of entries in `objects` are non-empty.
    //
    // For a global extract, this will have some overhead of repeatedly
    // setting up/tearing down threads. In that case, it would be 
    // better to assign chunks of `objects` to each thread.
    //
    // That's a future performance improvement, so deferring for now.
    //
    // Cluster by parent zoom, so that a subsequent search can find a
    // contiguous range of entries for any tile at zoom 6 or higher.
    // i.e. compare x then y at each zoom from CLUSTER_ZOOM to indexZoom,
    // which is the same as comparing the bit-interleaved (x, y) key.
    if (indexZoom < CLUSTER_ZOOM) {
        collectLowZoom();
        return; // the zoom-loop comparator treats all objects as equal
    }

    // Levels with a shift of Z6OffsetBits or more compare 0 with 0.
    const unsigned B = std::min<unsigned>(indexZoom - CLUSTER_ZOOM, Z6OffsetBits);
#ifndef NDEBUG
    checkInterleaveKey();
#endif
    auto key = [B](const OO& o) -> uint32_t { return interleaveKey(o.x, o.y, B); };
    const size_t n = end - begin;

    if (n < countingSortMinSize) {
        collectLowZoom();
        boost::sort::pdqsort(begin, end, [&key](const OO& a, const OO& b) { return key(a) < key(b); });
        return;
    }

    // Parallel counting sort on the key: each chunk counts its keys, then
    // scatters its objects into their final positions in a buffer, which is
    // copied back in order. Writing the objects (which can live in the
    // --store files) sequentially means each page is dirtied only once.
    const size_t chunks = std::max<size_t>(1, std::min<size_t>(threadNum, n / countingSortMinSize));
    const size_t chunkSize = (n + chunks - 1) / chunks;
    // OO has no default constructor, so use raw storage for the copies.
    static_assert(std::is_trivially_copyable<OO>::value && std::is_trivially_destructible<OO>::value,
        "the counting sort buffer copies objects as raw storage");
    static_assert(sizeof(OO) % sizeof(uint64_t) == 0 && alignof(OO) <= alignof(uint64_t), "the buffer is kept as words");
    const size_t words = n * (sizeof(OO) / sizeof(uint64_t));
    if (sortBufferWords < words) {
        sortBuffer.reset();
        sortBuffer.reset(new uint64_t[words]);
        sortBufferWords = words;
    }
    OO* buffer = reinterpret_cast<OO*>(sortBuffer.get());
    std::vector<size_t>& offsets = sortOffsets;
    offsets.assign(chunks * keyValues, 0);

    // The three steps below, each on all chunks; the chunks' threads are started once
    std::vector<std::function<void(size_t, size_t, size_t)>> steps;
    auto runChunks = [&](const std::function<void(size_t, size_t, size_t)>& fn) { steps.push_back(fn); };

    std::vector<std::vector<OO>> lowZoomChunks(lowZoom ? chunks : 0);
    runChunks([&](size_t c, size_t from, size_t to) {
        size_t* counts = &offsets[c * keyValues];
        auto it = begin + from;
        if (lowZoom) {
            std::vector<OO>& low = lowZoomChunks[c];
            for (size_t i = from; i < to; i++, ++it) {
                counts[key(*it)]++;
                if (it->oo.minZoom < CLUSTER_ZOOM) low.push_back(*it);
            }
        } else {
            for (size_t i = from; i < to; i++, ++it)
                counts[key(*it)]++;
        }
    });
    // Between the first two steps, on one thread
    auto between = [&]() {
    if (lowZoom) {
        size_t lowTotal = lowZoom->size();
        for (const auto& low : lowZoomChunks) lowTotal += low.size();
        lowZoom->reserve(lowTotal);
        for (auto& low : lowZoomChunks) {
            lowZoom->insert(lowZoom->end(), low.begin(), low.end());
            std::vector<OO>().swap(low);
        }
    }

    // Turn counts into starting positions: by key, then by chunk.
    size_t pos = 0;
    for (size_t k = 0; k < keyValues; k++)
        for (size_t c = 0; c < chunks; c++) {
            size_t& slot = offsets[c * keyValues + k];
            const size_t count = slot;
            slot = pos;
            pos += count;
        }
    };

    runChunks([&](size_t c, size_t from, size_t to) {
        size_t* next = &offsets[c * keyValues];
        auto it = begin + from;
        for (size_t i = from; i < to; i++, ++it)
            new (&buffer[next[key(*it)]++]) OO(*it);
    });

    runChunks([&](size_t c, size_t from, size_t to) {
        auto it = begin + from;
        for (size_t i = from; i < to; i++, ++it)
            *it = buffer[i];
    });

    // Run the steps: chunk c on thread c (this thread is chunk 0), all chunks of a step done
    // before the next step starts
    auto runChunk = [&](size_t c, SortBarrier* barrier) {
        const size_t from = std::min(n, c * chunkSize), to = std::min(n, from + chunkSize);
        for (size_t step = 0; step < steps.size(); step++) {
            steps[step](c, from, to);
            if (barrier) barrier->arrive();
            if (step == 0) {
                if (c == 0) between();
                if (barrier) barrier->arrive();
            }
        }
    };
    if (chunks == 1) {
        runChunk(0, nullptr);
    } else {
        SortBarrier barrier(chunks);
        std::vector<std::thread> threads;
        threads.reserve(chunks - 1);
        for (size_t c = 1; c < chunks; c++) threads.emplace_back(runChunk, c, &barrier);
        runChunk(0, &barrier);
        for (auto& t : threads) t.join();
    }
}

template void sortOutputObjects<OutputObjectXY>(
    const unsigned int indexZoom,
    const size_t threadNum,
	typename AppendVectorNS::AppendVector<OutputObjectXY>::Iterator begin,
	typename AppendVectorNS::AppendVector<OutputObjectXY>::Iterator end,
	std::vector<OutputObjectXY>* lowZoom
);

template void sortOutputObjects<OutputObjectXYID>(
    const unsigned int indexZoom,
    const size_t threadNum,
	typename AppendVectorNS::AppendVector<OutputObjectXYID>::Iterator begin,
	typename AppendVectorNS::AppendVector<OutputObjectXYID>::Iterator end,
	std::vector<OutputObjectXYID>* lowZoom
);

namespace {
struct AttributeKeyCacheEntry {
	AttributeIndex index;
	uint64_t key;
};
// Direct-mapped, per thread: neighbouring tiles share most attribute sets
// 64K entries (1 MB): the sets of neighbouring tiles stay cached
const size_t ATTRIBUTE_KEY_CACHE_SIZE = size_t(1) << 16;
thread_local std::vector<AttributeKeyCacheEntry> tlsAttributeKeys;
thread_local const AttributeStore* tlsAttributeKeysStore = nullptr;

// What the comparator below looks at first, as one 128-bit number: layer (8 bits), z_order
// in the layer's sort direction (16), geomType (2), attribute content hash (64); and the
// position of the object in the input.
struct SortEntry {
	uint64_t hi, lo;
	uint32_t index;
};
thread_local std::vector<SortEntry> tlsSortEntries;
thread_local std::vector<OutputObjectID> tlsSortedObjects;

uint64_t attributeKey(const AttributeStore& attributeStore, AttributeIndex index) {
	if (tlsAttributeKeysStore != &attributeStore) {
		tlsAttributeKeys.assign(ATTRIBUTE_KEY_CACHE_SIZE, AttributeKeyCacheEntry{~0u, 0});
		tlsAttributeKeysStore = &attributeStore;
	}
	uint32_t slot = index;
	slot ^= slot >> 16; slot *= 0x7feb352dU; slot ^= slot >> 15;
	AttributeKeyCacheEntry& entry = tlsAttributeKeys[slot & (ATTRIBUTE_KEY_CACHE_SIZE - 1)];
	if (entry.index != index)
		entry = AttributeKeyCacheEntry{index, attributeStore.contentHashUnsafe(index)};
	return entry.key;
}
}

static void sortOutputObjectIDsByHash(
	const std::vector<bool>& sortOrders,
	const AttributeStore& attributeStore,
	std::vector<OutputObjectID>& data
) {
	// Lexicographic comparison, with the order of: layer, geomType, attributes, id and objectID.
	// Note that attributes is preferred to objectID.
	// It is to arrange objects with the identical attributes continuously.
	// Such objects will be merged into one object, to reduce the size of output.
	//
	// Attribute sets are compared by content, not by AttributeIndex: the index is
	// handed out in insertion order, which depends on thread scheduling. The same
	// goes for the objectID of stored geometries, so the OSM id goes first;
	// TileDataSource::orderStoredGeometryTies handles the remaining ties.
	//
	// The comparison is done on a 128-bit key holding layer, z_order (flipped for a descending
	// layer), geomType and the attribute content hash, in this order of significance; when the
	// keys are equal, the rest of the comparison (content, index, id, objectID) as before.
	if (data.size() < 2) return;
	std::vector<SortEntry>& entries = tlsSortEntries;
	entries.resize(data.size());
	for (size_t i = 0; i < data.size(); i++) {
		const OutputObject& o = data[i].oo;
		// ascending: z_order with the sign bit flipped, so it orders as unsigned; descending: inverted
		uint64_t z = uint16_t(o.z_order) ^ 0x8000u;
		if (!sortOrders[o.layer]) z = 0xffffu - z;
		const uint64_t key = attributeKey(attributeStore, o.attributes);
		entries[i].hi = uint64_t(o.layer) << 56 | z << 40 | uint64_t(o.geomType) << 38 | key >> 26;
		entries[i].lo = key << 38;
		entries[i].index = i;
	}

	boost::sort::pdqsort(entries.begin(), entries.end(), [&data, &attributeStore](const SortEntry& a, const SortEntry& b) -> bool {
		if (a.hi != b.hi) return a.hi < b.hi;
		if (a.lo != b.lo) return a.lo < b.lo;
		// same layer, z_order, geomType and attribute hash
		const OutputObjectID& p = data[a.index];
		const OutputObjectID& q = data[b.index];
		const OutputObject& x = p.oo;
		const OutputObject& y = q.oo;
		if (x.attributes != y.attributes) {
			const int c = attributeStore.compareContentUnsafe(x.attributes, y.attributes);
			if (c != 0) return c < 0;
			return x.attributes < y.attributes;
		}
		if (p.id != q.id) return p.id < q.id;
		return x.objectID < y.objectID;
	});

	std::vector<OutputObjectID>& sorted = tlsSortedObjects;
	sorted.clear();
	sorted.reserve(data.size());
	for (const SortEntry& e : entries)
		sorted.push_back(data[e.index]);
	std::copy(sorted.begin(), sorted.end(), data.begin());
	if (sorted.capacity() > (1u << 20)) { std::vector<OutputObjectID>().swap(sorted); std::vector<SortEntry>().swap(entries); }
}


namespace {
// Per call: the attribute sets of the objects being sorted, in the comparator's order (content
// hash, content, index), so each object compares by the position of its set (rank)
struct RankSlot { uint32_t gen, index, rank; uint64_t hash; };
thread_local std::vector<RankSlot> tlsRankSlots;
thread_local uint32_t tlsRankGen = 0;
thread_local std::vector<uint32_t> tlsRankSets;    // slots of the distinct sets
thread_local std::vector<uint32_t> tlsObjectSlot;  // per object, its set's slot
// layer, z_order, geomType and the rank of the attribute set (hi), id (lo), objectID << 28 | position (oid)
struct RankEntry {
	uint64_t hi, lo, oid;
};
thread_local std::vector<RankEntry> tlsRankEntries;
const unsigned RANK_POS_BITS = 28;
#ifndef RANK_MIN_SIZE
#define RANK_MIN_SIZE 64
#endif

inline uint32_t rankSlotHash(uint32_t x) {
	x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
	return x;
}
}

void sortOutputObjectIDs(
	const std::vector<bool>& sortOrders,
	const AttributeStore& attributeStore,
	std::vector<OutputObjectID>& data
) {
	// The order of sortOutputObjectIDsByHash (see there), computed differently: the attribute sets of
	// the objects are ranked first, by content hash, then content, then AttributeIndex - the order
	// in which that comparator puts objects that agree in layer, z_order and geomType. The objects
	// are then sorted on (layer, z_order, geomType, rank), id, objectID: the comparator gives the
	// same answer for every pair as the original one (equal rank means the same AttributeIndex), so
	// the sort, which only depends on the comparator's answers, puts every object where it did,
	// without touching the objects or the attribute store while sorting.
	const size_t n = data.size();
	if (n < 2) return;
	// Small inputs (most tiles at z13 and z14): comparing hashes directly is cheaper than ranking the
	// sets first (same order)
	if (n < RANK_MIN_SIZE || n >= (size_t(1) << RANK_POS_BITS)) { sortOutputObjectIDsByHash(sortOrders, attributeStore, data); return; }

	// Distinct sets
	std::vector<RankSlot>& slots = tlsRankSlots;
	if (slots.empty()) slots.assign(64, RankSlot{0, 0, 0, 0});
	if (++tlsRankGen == 0) { for (RankSlot& s : slots) s.gen = 0; tlsRankGen = 1; }
	uint32_t gen = tlsRankGen;
	std::vector<uint32_t>& sets = tlsRankSets;
	sets.clear();
	std::vector<uint32_t>& objectSlot = tlsObjectSlot;
	objectSlot.resize(n);
	size_t mask = slots.size() - 1;
	for (size_t i = 0; i < n; i++) {
		const uint32_t index = data[i].oo.attributes;
		size_t s = rankSlotHash(index) & mask;
		while (slots[s].gen == gen && slots[s].index != index) s = (s + 1) & mask;
		if (slots[s].gen != gen) {
			if ((sets.size() + 1) * 2 > slots.size()) {
				// grow: rehash the distinct sets found so far, and the objects' slots
				std::vector<RankSlot> old;
				old.swap(slots);
				slots.assign(old.size() * 2, RankSlot{0, 0, 0, 0});
				if (++tlsRankGen == 0) tlsRankGen = 1;
				gen = tlsRankGen;
				mask = slots.size() - 1;
				std::vector<uint32_t> moved(old.size());
				for (uint32_t& k : sets) {
					size_t t = rankSlotHash(old[k].index) & mask;
					while (slots[t].gen == gen) t = (t + 1) & mask;
					slots[t] = old[k];
					slots[t].gen = gen;
					moved[k] = t;
					k = t;
				}
				for (size_t j = 0; j < i; j++) objectSlot[j] = moved[objectSlot[j]];
				s = rankSlotHash(index) & mask;
				while (slots[s].gen == gen) s = (s + 1) & mask;
			}
			slots[s] = RankSlot{gen, index, 0, attributeKey(attributeStore, index)};
			sets.push_back(s);
		}
		objectSlot[i] = s;
	}

	// Rank them: hash, then content, then index (as the comparator below)
	if (sets.size() > 1)
		std::sort(sets.begin(), sets.end(), [&slots, &attributeStore](uint32_t a, uint32_t b) {
			const RankSlot& x = slots[a];
			const RankSlot& y = slots[b];
			if (x.hash != y.hash) return x.hash < y.hash;
			const int c = attributeStore.compareContentUnsafe(x.index, y.index);
			if (c != 0) return c < 0;
			return x.index < y.index;
		});
	for (uint32_t r = 0; r < sets.size(); r++) slots[sets[r]].rank = r;

	std::vector<RankEntry>& entries = tlsRankEntries;
	entries.resize(n);
	for (size_t i = 0; i < n; i++) {
		const OutputObject& o = data[i].oo;
		// ascending: z_order with the sign bit flipped, so it orders as unsigned; descending: inverted
		uint64_t z = uint16_t(o.z_order) ^ 0x8000u;
		if (!sortOrders[o.layer]) z = 0xffffu - z;
		entries[i].hi = uint64_t(o.layer) << 56 | z << 40 | uint64_t(o.geomType) << 38 | slots[objectSlot[i]].rank;
		entries[i].lo = data[i].id;
		entries[i].oid = uint64_t(o.objectID) << RANK_POS_BITS | i;
	}

	boost::sort::pdqsort(entries.begin(), entries.end(), [](const RankEntry& a, const RankEntry& b) -> bool {
		if (a.hi != b.hi) return a.hi < b.hi;
		if (a.lo != b.lo) return a.lo < b.lo;
		return (a.oid >> RANK_POS_BITS) < (b.oid >> RANK_POS_BITS);
	});

	std::vector<OutputObjectID>& sorted = tlsSortedObjects;
	sorted.clear();
	sorted.reserve(n);
	const uint64_t posMask = (uint64_t(1) << RANK_POS_BITS) - 1;
	for (const RankEntry& e : entries)
		sorted.push_back(data[e.oid & posMask]);
	std::copy(sorted.begin(), sorted.end(), data.begin());
	if (sorted.capacity() > (1u << 20)) {
		std::vector<OutputObjectID>().swap(sorted); std::vector<RankEntry>().swap(entries); std::vector<uint32_t>().swap(objectSlot);
	}
	if (slots.size() > (1u << 16)) { std::vector<RankSlot>().swap(slots); std::vector<uint32_t>().swap(sets); }
}

void sortTileKeys(uint64_t *keys, size_t count, size_t threadNum) {
	boost::sort::sample_sort(keys, keys + count, std::less<uint64_t>(), std::max<size_t>(1, threadNum));
}