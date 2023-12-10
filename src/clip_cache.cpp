#include "clip_cache.h"
#include "coordinates_geom.h"

ClipCache::ClipCache(size_t threadNum, unsigned int baseZoom):
	baseZoom(baseZoom),
	clipCache(threadNum * 4),
	clipCacheMutex(threadNum * 4),
	clipCacheSize(threadNum * 4) {
}

const std::shared_ptr<MultiPolygon> ClipCache::get(uint zoom, TileCoordinate x, TileCoordinate y, NodeID objectID) const {
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

void ClipCache::add(const TileBbox& box, const NodeID objectID, const MultiPolygon& mp) {
	// The point of caching is to reuse the clip, so caching at the terminal zoom is
	// pointless.
	if (box.zoom == baseZoom)
		return;

	std::shared_ptr<MultiPolygon> copy = std::make_shared<MultiPolygon>();
	boost::geometry::assign(*copy, mp);

	size_t index = objectID % clipCacheMutex.size();
	std::lock_guard<std::mutex> lock(clipCacheMutex[index]);
	auto& cache = clipCache[index];
	// In a perfect world, this would be an LRU cache and we'd evict old entries
	// that are unlikely to be used again.
	//
	// But for now, just reset the cache every so often to prevent it growing
	// without bound.
	clipCacheSize[index]++;
	if (clipCacheSize[index] > 5000) {
		clipCacheSize[index] = 0;
		cache.clear();
	}

	cache[std::make_tuple(box.zoom, box.index, objectID)] = copy;
}
