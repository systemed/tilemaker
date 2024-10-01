/*! \file */ 
#ifndef _SHP_MEM_TILES
#define _SHP_MEM_TILES

#include "tile_data.h"

extern bool verbose;

class ShpMemTiles : public TileDataSource
{
public:
	ShpMemTiles(size_t threadNum, uint indexZoom);

	std::string name() const override { return "shp"; }

	void CreateNamedLayerIndex(const std::string& layerName);

	// Used in shape file loading
	void StoreGeometry(
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
	bool mayIntersect(const std::string& layerName, const Box& box) const;
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
	std::mutex indexMutex;


	// This differs from indexZoom. indexZoom is clamped to z14, as there is a noticeable
	// step function increase in memory use to go to higher zooms. For the
	// bitset index, the increase in memory is not as significant.
	unsigned int spatialIndexZoom;

	// The map is from layer name to a sparse vector of tiles that might have shapes.
	//
	// The outer vector has an entry for each z6 tile. The inner vector is a bitset,
	// indexed at spatialIndexZoom, where a bit is set if the z15 tiles at
	// 2 * (x*width + y) might contain at least one shape.
	// This is approximated by using the bounding boxes of the shapes. For large, irregular shapes, or
	// shapes with holes, the bounding box may result in many false positives. The first time the index
	// is consulted for a given tile, we'll do a more expensive intersects query to refine the index.
	// This lets us quickly reject negative Intersects queryes
	mutable std::map<std::string, std::vector<std::vector<bool>>> bitIndices;
};

#endif //_OSM_MEM_TILES

