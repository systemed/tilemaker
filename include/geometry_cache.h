#ifndef _GEOMETRY_CACHE_H
#define _GEOMETRY_CACHE_H

// A small class to map from ID -> geometry, with
// a bounded size and LRU-ish behaviour.
//
// Not safe for multi-threaded use!

#include "coordinates.h"
#include <memory>

#include <vector>

template <class T>
class GeometryCache {
public:
	// 64 x 32 entries: the cache only lives for one tile batch (see clear()), so a
	// smaller table loses few hits and pins much less heap in every tile thread.
	// Keep bucket size small so linear search is fast.
	enum : size_t { NUM_BUCKETS = 64, BUCKET_SIZE = 32 };

	GeometryCache():
		offsets(NUM_BUCKETS),
		filled(NUM_BUCKETS),
		ids(NUM_BUCKETS * BUCKET_SIZE),
		geometries(NUM_BUCKETS * BUCKET_SIZE) {}

	T* get(NodeID objectID) {
		const size_t bucket = objectID % NUM_BUCKETS;

		for (size_t i = bucket * BUCKET_SIZE; i < (bucket + 1) * BUCKET_SIZE; i++)
			if (ids[i] == objectID)
				return geometries[i].get();

		return nullptr;
	}

	void add(NodeID objectID, std::shared_ptr<T> geom) {
		const size_t bucket = objectID % NUM_BUCKETS;
		size_t offset = bucket * BUCKET_SIZE + offsets[bucket];
		geometries[offset] = std::move(geom);
		ids[offset] = objectID;
		if (filled[bucket] == 0)
			dirtyBuckets.push_back(bucket);
		if (filled[bucket] < BUCKET_SIZE)
			filled[bucket]++;
		offsets[bucket]++;

		if (offsets[bucket] == BUCKET_SIZE)
			offsets[bucket] = 0;
	}

	// Drop all entries (and release their geometries). Only touches the
	// buckets used since the last clear, so it is cheap on a lightly used cache.
	void clear() {
		for (size_t bucket : dirtyBuckets) {
			const size_t start = bucket * BUCKET_SIZE;
			for (size_t i = start; i < start + filled[bucket]; i++) {
				ids[i] = 0;
				geometries[i].reset();
			}
			filled[bucket] = 0;
			offsets[bucket] = 0;
		}
		dirtyBuckets.clear();
	}

private:
	std::vector<uint8_t> offsets;
	std::vector<uint8_t> filled;          // used slots per bucket since clear()
	std::vector<size_t> dirtyBuckets;     // buckets with filled > 0
	std::vector<NodeID> ids;
	// CONSIDER: we could use a raw pointer - getOrBuildLinestring controls the lifetime
	std::vector<std::shared_ptr<Linestring>> geometries;
};

#endif
