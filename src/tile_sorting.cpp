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

// The interleaved key has 2 * Z6OffsetBits bits; it is sorted by a single
// counting-sort pass over all possible key values.
static const size_t keyValues = size_t(1) << (2 * Z6OffsetBits);
// Below this, a comparison sort on the key is cheaper than counting.
static const size_t countingSortMinSize = 65536;

template<typename OO> void sortOutputObjects(
    const unsigned int indexZoom,
    const size_t threadNum,
	typename AppendVectorNS::AppendVector<OO>::Iterator begin,
	typename AppendVectorNS::AppendVector<OO>::Iterator end
)
{
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
    if (indexZoom < CLUSTER_ZOOM)
        return; // the zoom-loop comparator treats all objects as equal

    // Levels with a shift of Z6OffsetBits or more compare 0 with 0.
    const unsigned B = std::min<unsigned>(indexZoom - CLUSTER_ZOOM, Z6OffsetBits);
#ifndef NDEBUG
    checkInterleaveKey();
#endif
    auto key = [B](const OO& o) -> uint32_t { return interleaveKey(o.x, o.y, B); };
    const size_t n = end - begin;

    if (n < countingSortMinSize) {
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
    std::unique_ptr<typename std::aligned_storage<sizeof(OO), alignof(OO)>::type[]> storage(
        new typename std::aligned_storage<sizeof(OO), alignof(OO)>::type[n]);
    OO* buffer = reinterpret_cast<OO*>(storage.get());
    std::vector<size_t> offsets(chunks * keyValues, 0);

    auto runChunks = [&](const std::function<void(size_t, size_t, size_t)>& fn) {
        boost::asio::thread_pool pool(chunks);
        for (size_t c = 0; c < chunks; c++) {
            const size_t from = c * chunkSize, to = std::min(n, from + chunkSize);
            boost::asio::post(pool, [&fn, c, from, to]() { fn(c, from, to); });
        }
        pool.join();
    };

    runChunks([&](size_t c, size_t from, size_t to) {
        size_t* counts = &offsets[c * keyValues];
        auto it = begin + from;
        for (size_t i = from; i < to; i++, ++it)
            counts[key(*it)]++;
    });

    // Turn counts into starting positions: by key, then by chunk.
    size_t pos = 0;
    for (size_t k = 0; k < keyValues; k++)
        for (size_t c = 0; c < chunks; c++) {
            size_t& slot = offsets[c * keyValues + k];
            const size_t count = slot;
            slot = pos;
            pos += count;
        }

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
}

template void sortOutputObjects<OutputObjectXY>(
    const unsigned int indexZoom,
    const size_t threadNum,
	typename AppendVectorNS::AppendVector<OutputObjectXY>::Iterator begin,
	typename AppendVectorNS::AppendVector<OutputObjectXY>::Iterator end
);

template void sortOutputObjects<OutputObjectXYID>(
    const unsigned int indexZoom,
    const size_t threadNum,
	typename AppendVectorNS::AppendVector<OutputObjectXYID>::Iterator begin,
	typename AppendVectorNS::AppendVector<OutputObjectXYID>::Iterator end
);

namespace {
struct AttributeKeyCacheEntry {
	AttributeIndex index;
	uint64_t key;
};
// Direct-mapped, per thread: neighbouring tiles share most attribute sets
const size_t ATTRIBUTE_KEY_CACHE_SIZE = 1 << 12;
thread_local std::vector<AttributeKeyCacheEntry> tlsAttributeKeys;
thread_local const AttributeStore* tlsAttributeKeysStore = nullptr;

struct KeyedOutputObjectID {
	uint64_t attributeKey;
	OutputObjectID o;
};

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

void sortOutputObjectIDs(
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
	std::vector<KeyedOutputObjectID> keyed;
	keyed.reserve(data.size());
	for (const OutputObjectID& o : data)
		keyed.push_back(KeyedOutputObjectID{attributeKey(attributeStore, o.oo.attributes), o});

	boost::sort::pdqsort(keyed.begin(), keyed.end(), [&sortOrders, &attributeStore](const KeyedOutputObjectID& a, const KeyedOutputObjectID& b) -> bool {
		const OutputObject& x = a.o.oo;
		const OutputObject& y = b.o.oo;
		if (x.layer < y.layer) return true;
		if (x.layer > y.layer) return false;
		if (x.z_order < y.z_order) return  sortOrders[x.layer];
		if (x.z_order > y.z_order) return !sortOrders[x.layer];
		if (x.geomType < y.geomType) return true;
		if (x.geomType > y.geomType) return false;
		if (x.attributes != y.attributes) {
			if (a.attributeKey != b.attributeKey) return a.attributeKey < b.attributeKey;
			const int c = attributeStore.compareContentUnsafe(x.attributes, y.attributes);
			if (c != 0) return c < 0;
			return x.attributes < y.attributes;
		}
		if (a.o.id != b.o.id) return a.o.id < b.o.id;
		return x.objectID < y.objectID;
	});

	for (size_t i = 0; i < keyed.size(); i++)
		data[i] = keyed[i].o;
}

void sortTileCoordinates(
    const size_t baseZoom,
    const size_t threadNum,
    std::deque<std::pair<unsigned int, TileCoordinates>>& tileCoordinates
)
{
    boost::sort::block_indirect_sort(
		tileCoordinates.begin(), tileCoordinates.end(), 
		[baseZoom](auto const &a, auto const &b) {
			const auto aZoom = a.first;
			const auto bZoom = b.first;
			const auto aX = a.second.x;
			const auto aY = a.second.y;
			const auto bX = b.second.x;
			const auto bY = b.second.y;
			const bool aLowZoom = aZoom < CLUSTER_ZOOM;
			const bool bLowZoom = bZoom < CLUSTER_ZOOM;

			// Breadth-first for z0..5
			if (aLowZoom != bLowZoom)
				return aLowZoom;

			if (aLowZoom && bLowZoom) {
				if (aZoom != bZoom)
					return aZoom < bZoom;

				if (aX != bX)
					return aX < bX;

				return aY < bY;
			}

			// Compare beyond baseZoom too, so tiles above baseZoom get a total,
			// depth-first order (parents before children) rather than ties.
			const size_t maxZoom = std::max<size_t>(baseZoom, std::max(aZoom, bZoom));
			for (size_t z = CLUSTER_ZOOM; z <= maxZoom; z++) {
				// Translate both a and b to zoom z, compare.
				// First, sanity check: can we translate it to this zoom?
				if (aZoom < z || bZoom < z) {
					return aZoom < bZoom;
				}

				const auto aXz = aX / (1 << (aZoom - z));
				const auto aYz = aY / (1 << (aZoom - z));
				const auto bXz = bX / (1 << (bZoom - z));
				const auto bYz = bY / (1 << (bZoom - z));

				if (aXz != bXz)
					return aXz < bXz;

				if (aYz != bYz)
					return aYz < bYz;
			}

			return false;
		}, 
		threadNum);
}