/*! \file */ 
#ifndef _SHP_MEM_TILES
#define _SHP_MEM_TILES

#include "tile_data.h"
#include "shared_data.h"

class ShpMemTiles : public TileDataSource
{
public:
	ShpMemTiles(uint baseZoom);

	virtual void GenerateTileListAtZoom(uint zoom, TileCoordinatesSet &dstCoords);

	virtual void GetTileData(TileCoordinates dstIndex, uint zoom, 
		std::vector<OutputObjectRef> &dstTile);

	// Find intersecting shapefile layer
	virtual std::vector<std::string> FindIntersecting(const std::string &layerName, Box &box) const;
	virtual bool Intersects(const std::string &layerName, Box &box) const;

	virtual uint GetBaseZoom();

	virtual void Load(class LayerDefinition &layers, 
		bool hasClippingBox,
		const Box &clippingBox);

	// Used in shape file loading
	virtual OutputObjectRef AddObject(uint_least8_t layerNum,
		const std::string &layerName, 
		enum OutputGeometryType geomType,
		Geometry geometry, 
		bool isIndexed, bool hasName, const std::string &name);

private:
	std::vector<uint> findIntersectingGeometries(const std::string &layerName, Box &box) const;
	std::vector<uint> verifyIntersectResults(std::vector<IndexValue> &results, Point &p1, Point &p2) const;
	std::vector<std::string> namesOfGeometries(std::vector<uint> &ids) const;

	/// Add an OutputObject to all tiles between min/max lat/lon
	void addToTileIndexByBbox(OutputObjectRef &oo, TileIndex &tileIndex,
		double minLon, double minLatp, double maxLon, double maxLatp);

	/// Add an OutputObject to all tiles along a polyline
	void addToTileIndexPolyline(OutputObjectRef &oo, TileIndex &tileIndex, Geometry *geom);

	virtual void CreateNamedLayerIndex(const std::string &layerName);

	uint baseZoom;
	TileIndex tileIndex;
	std::vector<Geometry> cachedGeometries;					// prepared boost::geometry objects (from shapefiles)
	std::map<uint, std::string> cachedGeometryNames;			//  | optional names for each one
	std::map<std::string, RTree> indices;			// Spatial indices, boost::geometry::index objects for shapefile indices
};

#endif //_OSM_MEM_TILES

