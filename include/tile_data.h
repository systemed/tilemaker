#ifndef _TILE_DATA_H
#define _TILE_DATA_H

#include <map>
#include <set>
#include <vector>
#include <memory>
#include "osm_store.h"
#include "output_object.h"

typedef std::vector<OutputObjectRef>::const_iterator OutputObjectsConstIt;
typedef std::map<TileCoordinates, std::vector<OutputObjectRef>, TileCoordinatesCompare > TileIndex;
typedef std::set<TileCoordinates, TileCoordinatesCompare> TileCoordinatesSet;

class ObjectsAtSubLayerIterator : public OutputObjectsConstIt
{
public:
	ObjectsAtSubLayerIterator(OutputObjectsConstIt it, const class TileData &tileData);

private:
	const class TileData &tileData;
};

typedef std::pair<ObjectsAtSubLayerIterator,ObjectsAtSubLayerIterator> ObjectsAtSubLayerConstItPair;

/**
 * Corresponds to a single tile at a single zoom level.
 */
class TilesAtZoomIterator : public TileCoordinatesSet::const_iterator
{
public:
	TilesAtZoomIterator(TileCoordinatesSet::const_iterator it, class TileData &tileData, uint zoom);

	TileCoordinates GetCoordinates() const;
	ObjectsAtSubLayerConstItPair GetObjectsAtSubLayer(uint_least8_t layer) const;

	TilesAtZoomIterator& operator++();

private:
	void RefreshData();

	class TileData &tileData;
	std::vector<OutputObjectRef> data;
	uint zoom;
};

/**
 * The tile worker process should access all map data through this class and its associated iterators.
 * This gives us room for future work on getting input data in a lazy fashion (in order to avoid
 * overwhelming memory resources.)
 */
class TileData
{
	friend ObjectsAtSubLayerIterator;
	friend TilesAtZoomIterator;

public:
	TileData(const TileIndex &tileIndexPbf, const TileIndex &tileIndexShp, uint baseZoom);

	class TilesAtZoomIterator GetTilesAtZoomBegin();
	class TilesAtZoomIterator GetTilesAtZoomEnd();
	size_t GetTilesAtZoomSize();

	void SetZoom(uint zoom);

private:
	const TileIndex &tileIndexPbf, &tileIndexShp;
	TileCoordinatesSet tileCoordinates;
	uint zoom, baseZoom;
};

#endif //_TILE_DATA_H
