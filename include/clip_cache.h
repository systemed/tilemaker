#ifndef _CLIP_CACHE_H
#define _CLIP_CACHE_H

#include "coordinates.h"
#include "coordinates_geom.h"
#include "geom.h"
#include <mutex>

class TileBbox;

template <class T>
class ClipCache {
public:
	ClipCache(size_t threadNum, unsigned int baseZoom):
		baseZoom(baseZoom),
		clipCache(threadNum * 16),
		clipCacheMutex(threadNum * 16),
		clipCacheSize(threadNum * 16) {
	}

	const std::shared_ptr<T> get(uint zoom, TileCoordinate x, TileCoordinate y, NodeID objectID) const{
		// Look for a previously clipped version at z-1, z-2, ...

		std::lock_guard<std::mutex> lock(clipCacheMutex[objectID % clipCacheMutex.size()]);
		while (zoom > 0) {
			zoom--;
			x /= 2;
			y /= 2;
			const auto& cache = clipCache[objectID % clipCache.size()];
			const auto& rv = cache.find(std::make_tuple(zoom, TileCoordinates(x, y), objectID));
			if (rv != cache.end()) {
				return rv->second;
			}
		}

		return nullptr;
	}

	void add(const TileBbox& bbox, const NodeID objectID, const T& output) {
		// The point of caching is to reuse the clip, so caching at the terminal zoom is
		// pointless.
		if (bbox.zoom == baseZoom)
			return;

		std::shared_ptr<T> copy = std::make_shared<T>();
		boost::geometry::assign(*copy, output);

		size_t index = objectID % clipCacheMutex.size();
		std::vector<std::shared_ptr<T>> objects;
		std::lock_guard<std::mutex> lock(clipCacheMutex[index]);
		auto& cache = clipCache[index];
		// Reset the cache periodically so it doesn't grow without bound.
		//
		// I also tried boost's lru_cache -- but it seemed to perform worse, maybe
		// due to the bookkeeping? We could try authoring a bounded map that
		// evicts in FIFO order, which will have less bookkeeping.
		clipCacheSize[index]++;
		if (clipCacheSize[index] > 1024) {
			clipCacheSize[index] = 0;
			// Copy the map's contents to a vector so that calling .clear()
			// and releasing the lock can happen separately from running the
			// destructors of all of the objects.
			objects.reserve(1025);
			for (const auto& x : cache)
				objects.push_back(x.second);
			cache.clear();
		}

		cache[std::make_tuple(bbox.zoom, bbox.index, objectID)] = copy;
	}

private:
	unsigned int baseZoom;
	std::vector<std::map<std::tuple<uint16_t, TileCoordinates, NodeID>, std::shared_ptr<T>>> clipCache;
	mutable std::vector<std::mutex> clipCacheMutex;
	std::vector<size_t> clipCacheSize;
};

#endif
