/*! \file */ 
#ifndef _SHP_MEM_TILES
#define _SHP_MEM_TILES

#include "tile_data.h"

extern bool verbose;

class ShpMemTiles : public TileDataSource
{
public:
	ShpMemTiles(size_t threadNum, uint baseZoom);

	void CreateNamedLayerIndex(const std::string& layerName);

	// Used in shape file loading
	void StoreShapefileGeometry(
		uint_least8_t layerNum,
		const std::string& layerName, 
		enum OutputGeometryType geomType,
		Geometry geometry, 
		bool isIndexed,
		bool hasName,
		const std::string& name,
		uint minzoom,
		AttributeIndex attrIdx
	);

	std::vector<uint> QueryMatchingGeometries(
		const std::string& layerName,
		bool once,
		Box& box, 
		std::function<std::vector<IndexValue>(const RTree& rtree)> indexQuery, 
		std::function<bool(const OutputObject& oo)> checkQuery
	) const;
	std::vector<std::string> namesOfGeometries(const std::vector<uint>& ids) const;

	template <typename GeometryT>
	double AreaIntersecting(const std::string& layerName, GeometryT& g) const {
		auto f = indices.find(layerName);
		if (f==indices.end()) { 
			if (verbose)
				std::cerr << "Couldn't find indexed layer " << layerName << std::endl; 
			return false;
		}
		Box box;
		geom::envelope(g, box);
		std::vector<IndexValue> results;
		f->second.query(geom::index::intersects(box), back_inserter(results));
		MultiPolygon mp, tmp;
		for (const auto &it : results) {
			OutputObject oo = indexedGeometries.at(it.second);
			if (oo.geomType!=POLYGON_) continue;
			geom::union_(mp, retrieveMultiPolygon(oo.objectID), tmp);
			geom::assign(mp, tmp);
		}
		geom::correct(mp);
		return geom::covered_by(g, mp);
	}

private:
	std::vector<OutputObject> indexedGeometries;				// prepared boost::geometry objects (from shapefiles)
	std::map<uint, std::string> indexedGeometryNames;			//  | optional names for each one
	std::map<std::string, RTree> indices;			// Spatial indices, boost::geometry::index objects for shapefile indices
};

#endif //_OSM_MEM_TILES

