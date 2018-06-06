#ifndef _TILE_DATA_H
#define _TILE_DATA_H

#include <map>
#include <vector>
#include <memory>
#include "osm_store.h"
#include "output_object.h"

typedef std::vector<OutputObjectRef>::const_iterator OutputObjectsConstIt;
typedef std::pair<OutputObjectsConstIt,OutputObjectsConstIt> OutputObjectsConstItPair;
typedef std::map<TileCoordinates, std::vector<OutputObjectRef>, TileCoordinatesCompare > TileIndex;

class ObjectsAtSubLayerIterator : public OutputObjectsConstIt
{
public:
	ObjectsAtSubLayerIterator(OutputObjectsConstIt it, const class TileData &tileData);

private:
	const class TileData &tileData;
};

typedef std::pair<ObjectsAtSubLayerIterator,ObjectsAtSubLayerIterator> ObjectsAtSubLayerConstItPair;

class TilesAtZoomIterator : public TileIndex::const_iterator
{
public:
	TilesAtZoomIterator(TileIndex::const_iterator it, class TileData &tileData);

	TileCoordinates GetCoordinates() const;
	ObjectsAtSubLayerConstItPair GetObjectsAtSubLayer(uint_least8_t layer) const;

private:
	class TileData &tileData;
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
	TileData();

	class TilesAtZoomIterator GetTilesAtZoomBegin();
	class TilesAtZoomIterator GetTilesAtZoomEnd();
	size_t GetTilesAtZoomSize();

	void SetTileIndexForZoom(const TileIndex *tileIndexForZoom);

private:
	const TileIndex *tileIndexForZoom;
};

#endif //_TILE_DATA_H
