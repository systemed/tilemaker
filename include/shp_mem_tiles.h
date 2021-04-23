/*! \file */ 
#ifndef _SHP_MEM_TILES
#define _SHP_MEM_TILES

#include "tile_data.h"

class ShpMemTiles : public TileDataSource
{
public:
	ShpMemTiles(OSMStore &osmStore, uint baseZoom);

	void CreateNamedLayerIndex(const std::string &layerName);

	// Used in shape file loading
	OutputObjectRef AddObject(uint_least8_t layerNum,
		const std::string &layerName, 
		enum OutputGeometryType geomType,
		Geometry geometry, 
		bool isIndexed, bool hasName, const std::string &name, AttributeStoreRef attributes);

	void AddObject(TileCoordinates const &index, OutputObjectRef const &oo) {
		tileIndex[index].push_back(oo);
	}
	std::vector<uint> QueryMatchingGeometries(const std::string &layerName, Box &box, 
		std::function<std::vector<IndexValue>(const RTree &rtree)> indexQuery, 
		std::function<bool(OutputObject &oo)> checkQuery) const;
	std::vector<std::string> namesOfGeometries(const std::vector<uint> &ids) const;

private:
	/// Add an OutputObject to all tiles between min/max lat/lon
	void addToTileIndexByBbox(OutputObjectRef &oo, 
		double minLon, double minLatp, double maxLon, double maxLatp);

	/// Add an OutputObject to all tiles along a polyline
	void addToTileIndexPolyline(OutputObjectRef &oo, Geometry *geom);

	OSMStore &osmStore;

	std::vector<OutputObjectRef> cachedGeometries;					// prepared boost::geometry objects (from shapefiles)
	std::map<uint, std::string> cachedGeometryNames;			//  | optional names for each one
	std::map<std::string, RTree> indices;			// Spatial indices, boost::geometry::index objects for shapefile indices
};

#endif //_OSM_MEM_TILES

