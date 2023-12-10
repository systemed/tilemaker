#ifndef _CLIP_CACHE_H
#define _CLIP_CACHE_H

#include "coordinates.h"
#include "geom.h"
#include <mutex>

class TileBbox;

class ClipCache {
public:
	ClipCache(size_t threadNum, unsigned int baseZoom);
	const std::shared_ptr<MultiPolygon> get(uint zoom, TileCoordinate x, TileCoordinate y, NodeID objectID) const;
	void add(const TileBbox& bbox, const NodeID objectID, const MultiPolygon& output);

private:
	unsigned int baseZoom;
	std::vector<std::map<std::tuple<uint16_t, TileCoordinates, NodeID>, std::shared_ptr<MultiPolygon>>> clipCache;
	mutable std::vector<std::mutex> clipCacheMutex;
	std::vector<size_t> clipCacheSize;
};

#endif
