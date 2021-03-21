/*! \file */ 
#ifndef _TILE_DATA_H
#define _TILE_DATA_H

#include <map>
#include <set>
#include <vector>
#include <memory>
#include <unordered_set>
#include "output_object.h"

typedef std::unordered_set<OutputObjectRef>::const_iterator OutputObjectsConstIt;
typedef std::map<TileCoordinates, std::vector<OutputObjectRef>, TileCoordinatesCompare > TileIndex;
typedef std::set<TileCoordinates, TileCoordinatesCompare> TileCoordinatesSet;

using OutputObjectsPerLayer = std::vector< std::unordered_set<OutputObjectRef> >;

void MergeTileCoordsAtZoom(uint zoom, uint baseZoom, const TileIndex &srcTiles, TileCoordinatesSet &dstCoords);
void MergeSingleTileDataAtZoom(TileCoordinates dstIndex, uint zoom, uint baseZoom, const TileIndex &srcTiles, OutputObjectsPerLayer &dstTile);

class TileDataSource {

public:
	///This must be thread safe!
	virtual void MergeTileCoordsAtZoom(uint zoom, TileCoordinatesSet &dstCoords)=0;

	///This must be thread safe!
	virtual void MergeSingleTileDataAtZoom(TileCoordinates dstIndex, uint zoom, OutputObjectsPerLayer &dstTile)=0;

	virtual std::vector<std::string> FindIntersecting(const std::string &layerName, Box &box)
	{
		return std::vector<std::string>();
	};

	virtual bool Intersects(const std::string &layerName, Box &box)
	{
		return false;
	};

	virtual void CreateNamedLayerIndex(const std::string &name) {};

	// Used in shape file loading
	virtual OutputObjectRef AddObject(uint_least8_t layerNum,
		const std::string &layerName, 
		enum OutputGeometryType geomType,
		Geometry geometry, 
		bool isIndexed, bool hasName, const std::string &name, AttributeStoreRef attributes) {return OutputObjectRef();};

	//Used in OSM data loading
	virtual void AddObject(TileCoordinates tileIndex, OutputObjectRef oo) {};
};

using ObjectsAtSubLayerIterator = OutputObjectsConstIt;
using ObjectsAtSubLayerConstItPair = std::pair<ObjectsAtSubLayerIterator,ObjectsAtSubLayerIterator>;

/**
 * Corresponds to a single tile at a single zoom level.
 */
class TilesAtZoomIterator : public TileCoordinatesSet::const_iterator {

	// Reserve the number of output objects
	enum { reserve_output_objects = 10000 };

public:
	TilesAtZoomIterator(TileCoordinatesSet::const_iterator it, class TileData &tileData, uint zoom);

	TileCoordinates GetCoordinates() const;
	bool HasObjectsAtSubLayer(uint_least8_t layer) const {
		return (layer < data.size()) && !data.at(layer).empty();
	}

	std::unordered_set<OutputObjectRef> const &GetObjectsAtSubLayer(uint_least8_t layer) const {
		return data.at(layer);
	}

	TilesAtZoomIterator& operator++();
	TilesAtZoomIterator operator++(int a);
	TilesAtZoomIterator& operator--();
	TilesAtZoomIterator operator--(int a);

private:
	void RefreshData();

	class TileData &tileData;
	OutputObjectsPerLayer data;
	uint zoom;
};

/**
 * The tile worker process should access all map data through this class and its associated iterators.
 * This gives us room for future work on getting input data in a lazy fashion (in order to avoid
 * overwhelming memory resources.)
 */
class TileData {

	friend ObjectsAtSubLayerIterator;
	friend TilesAtZoomIterator;

public:
	TileData(std::vector<class TileDataSource *> const &sources, uint zoom);

	class TilesAtZoomIterator GetTilesAtZoomBegin();
	class TilesAtZoomIterator GetTilesAtZoomEnd();
	size_t GetTilesAtZoomSize() const;

private:
	std::vector<class TileDataSource *> const &sources;
	TileCoordinatesSet tileCoordinates;
	uint zoom;
};

#endif //_TILE_DATA_H
