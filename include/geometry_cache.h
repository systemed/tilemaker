#ifndef _GEOMETRY_CACHE_H
#define _GEOMETRY_CACHE_H

// A small class to map from ID -> geometry, with
// a bounded size and LRU-ish behaviour.
//
// Not safe for multi-threaded use!

#include "coordinates.h"
#include <memory>

#define NUM_BUCKETS 256
// Keep bucket size small so linear search is fast
#define BUCKET_SIZE 32


template <class T>
class GeometryCache {
public:
	GeometryCache():
		offsets(NUM_BUCKETS),
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
		geometries[offset] = geom;
		ids[offset] = objectID;
		offsets[bucket]++;

		if (offsets[bucket] == BUCKET_SIZE)
			offsets[bucket] = 0;
	}

private:
	std::vector<uint8_t> offsets;
	std::vector<NodeID> ids;
	// CONSIDER: we could use a raw pointer - getOrBuildLinestring controls the lifetime
	std::vector<std::shared_ptr<Linestring>> geometries;
};

#endif
