#ifndef _TILE_DATA_H
#define _TILE_DATA_H

#include <map>
#include <set>
#include <vector>
#include <memory>
#include "output_object.h"

typedef std::vector<OutputObjectRef>::const_iterator OutputObjectsConstIt;
typedef std::map<TileCoordinates, std::vector<OutputObjectRef>, TileCoordinatesCompare > TileIndex;
typedef std::set<TileCoordinates, TileCoordinatesCompare> TileCoordinatesSet;

void MergeTileCoordsAtZoom(uint zoom, uint baseZoom, const TileIndex &srcTiles, TileCoordinatesSet &dstCoords);
void MergeSingleTileDataAtZoom(TileCoordinates dstIndex, uint zoom, uint baseZoom, const TileIndex &srcTiles, 
	std::vector<OutputObjectRef> &dstTile);

class TileDataSource
{
public:
	///This must be thread safe!
	virtual void MergeTileCoordsAtZoom(uint zoom, TileCoordinatesSet &dstCoords)=0;

	///This must be thread safe!
	virtual void MergeSingleTileDataAtZoom(TileCoordinates dstIndex, uint zoom, 
		std::vector<OutputObjectRef> &dstTile)=0;

	virtual std::vector<std::string> FindIntersecting(const std::string &layerName, Box &box)
	{
		return std::vector<std::string>();
	};

	virtual bool Intersects(const std::string &layerName, Box &box)
	{
		return false;
	};

	virtual void CreateNamedLayerIndex(const std::string &name) {};

};

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
	TilesAtZoomIterator operator++(int a);
	TilesAtZoomIterator& operator--();
	TilesAtZoomIterator operator--(int a);

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
	TileData(const std::vector<class TileDataSource *> sources);

	class TilesAtZoomIterator GetTilesAtZoomBegin();
	class TilesAtZoomIterator GetTilesAtZoomEnd();
	size_t GetTilesAtZoomSize();

	void SetZoom(uint zoom);

private:
	const std::vector<class TileDataSource *> sources;
	TileCoordinatesSet tileCoordinates;
	uint zoom;
};

#endif //_TILE_DATA_H
