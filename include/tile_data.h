/*! \file */ 
#ifndef _TILE_DATA_H
#define _TILE_DATA_H

#include <map>
#include <set>
#include <vector>
#include <memory>
#include "output_object.h"

typedef std::vector<OutputObjectRef>::const_iterator OutputObjectsConstIt;
typedef std::map<TileCoordinates, std::vector<OutputObjectRef>, TileCoordinatesCompare > TileIndexRaw;
typedef std::set<TileCoordinates, TileCoordinatesCompare> TileCoordinatesSet;

class TileIndex
{
public:
	TileIndex(uint baseZoom);
	virtual ~TileIndex();

	void GenerateTileList(uint destZoom, TileCoordinatesSet &dstCoords) const;
	void GetTileData(TileCoordinates dstIndex, uint destZoom, 
		std::vector<OutputObjectRef> &dstTile) const;

	uint GetBaseZoom() const;

	void Add(TileCoordinates tileIndex, OutputObjectRef oo);
	void Add(OutputObjectRef &oo, Point pt);
	void AddByBbox(OutputObjectRef &oo, 
		                      double minLon, double minLatp, double maxLon, double maxLatp);
	void AddByPolyline(OutputObjectRef &oo, Geometry *geom);

private:
	TileIndexRaw index;
	const uint baseZoom;
};

// ***********************************

class TileIndexCached : public TileIndex
{
public:
	TileIndexCached(uint baseZoom);
	virtual ~TileIndexCached();

	OutputObjectRef AddObject(uint_least8_t layerNum,
		const std::string &layerName, enum OutputGeometryType geomType,
		Geometry geometry, bool isIndexed, bool hasName, const std::string &name);
	
	std::vector<std::shared_ptr<Geometry> > cachedGeometries;   // prepared boost::geometry objects (from shapefiles)
	std::map<uint, std::string> cachedGeometryNames;			//  | optional names for each one
	std::map<std::string, RTree> indices;			// Spatial indices, boost::geometry::index objects for shapefile indices

	void CreateNamedLayerIndex(const std::string &layerName);

	std::vector<uint> findIntersectingGeometries(const std::string &layerName, Box &box) const;
	std::vector<uint> verifyIntersectResults(std::vector<IndexValue> &results, Point &p1, Point &p2) const;
	std::vector<std::string> namesOfGeometries(std::vector<uint> &ids) const;

};

// ***********************************


class TileDataSource
{
public:
	///This must be thread safe!
	virtual void GenerateTileListAtZoom(uint zoom, TileCoordinatesSet &dstCoords)=0;

	///This must be thread safe!
	virtual void GetTileData(TileCoordinates dstIndex, uint zoom, 
		std::vector<OutputObjectRef> &dstTile)=0;

	virtual std::vector<std::string> FindIntersecting(const std::string &layerName, Box &box) const
	{
		return std::vector<std::string>();
	};

	virtual bool Intersects(const std::string &layerName, Box &box) const
	{
		return false;
	};

	//Used in OSM data loading
	virtual void AddObject(TileCoordinates tileIndex, OutputObjectRef oo) {};

	virtual uint GetBaseZoom()=0;
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
 * \brief Corresponds to a single tile at a single zoom level.
 *
 * This class is NOT shared between threads.
 */
class TilesAtZoomIterator : public TileCoordinatesSet::const_iterator
{
public:
	TilesAtZoomIterator(TileCoordinatesSet::const_iterator it, class TileData &tileData, uint zoom);

	TileCoordinates GetCoordinates() const;
	ObjectsAtSubLayerConstItPair GetObjectsAtSubLayer(uint_least8_t layer);

	TilesAtZoomIterator& operator++();
	TilesAtZoomIterator operator++(int a);
	TilesAtZoomIterator& operator--();
	TilesAtZoomIterator operator--(int a);

private:
	void RefreshData();

	class TileData &tileData;
	std::vector<OutputObjectRef> data;
	uint zoom;
	bool ready;
};

/**
 * The tile worker process should access all map data through this class and its associated iterators.
 * This gives us room for future work on getting input data in a lazy fashion (in order to avoid
 * overwhelming memory resources.)
 *
 * This class IS shared between threads.
 */
class TileData
{
	friend ObjectsAtSubLayerIterator;
	friend TilesAtZoomIterator;

public:
	TileData(const std::vector<class TileDataSource *> sources);

	///Must be thread safe!
	class TilesAtZoomIterator GetTilesAtZoomBegin();

	///Must be thread safe!
	class TilesAtZoomIterator GetTilesAtZoomEnd();

	///Must be thread safe!
	size_t GetTilesAtZoomSize();

	void SetZoom(uint zoom);

private:
	const std::vector<class TileDataSource *> sources;
	TileCoordinatesSet tileCoordinates;
	uint zoom;
};

#endif //_TILE_DATA_H

