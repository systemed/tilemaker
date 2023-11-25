#include "shp_mem_tiles.h"
#include <iostream>
using namespace std;
namespace geom = boost::geometry;
extern bool verbose;

ShpMemTiles::ShpMemTiles(uint baseZoom)
	: TileDataSource(baseZoom)
{ }

// Look for shapefile objects that fulfil a spatial query (e.g. intersects)
// Parameters:
// - shapefile layer name to search
// - bounding box to match against
// - indexQuery(rtree, results) lambda, implements: rtree.query(geom::index::covered_by(box), back_inserter(results))
// - checkQuery(osmstore, id) lambda, implements:   return geom::covered_by(osmStore.retrieve(id), geom)
vector<uint> ShpMemTiles::QueryMatchingGeometries(
	const string& layerName,
	bool once,
	Box& box,
	function<vector<IndexValue>(const RTree &rtree)> indexQuery,
	function<bool(const OutputObject& oo)> checkQuery
) const {
	
	// Find the layer
	auto f = indices.find(layerName); // f is an RTree
	if (f==indices.end()) {
		if (verbose) cerr << "Couldn't find indexed layer " << layerName << endl;
		return vector<uint>();	// empty, relations not supported
	}
	
	// Run the index query
	vector<IndexValue> results = indexQuery(f->second);
	
	// Run the check query
	vector<uint> ids;
	for (auto it: results) {
		uint id = it.second;
		if (checkQuery(indexedGeometries.at(id))) { ids.push_back(id); if (once) break; }
	}
	return ids;
}

vector<string> ShpMemTiles::namesOfGeometries(const vector<uint>& ids) const {
	vector<string> names;
	for (uint i=0; i<ids.size(); i++) {
		if (indexedGeometryNames.find(ids[i])!=indexedGeometryNames.end()) {
			names.push_back(indexedGeometryNames.at(ids[i]));
		}
	}
	return names;
}

void ShpMemTiles::CreateNamedLayerIndex(const std::string& layerName) {
	indices[layerName]=RTree();
}

void ShpMemTiles::StoreShapefileGeometry(
	uint_least8_t layerNum,
	const std::string& layerName,
	enum OutputGeometryType geomType,
	Geometry geometry,
	bool isIndexed,
	bool hasName,
	const std::string& name, 
	uint minzoom,
	AttributeIndex attrIdx
) {

	geom::model::box<Point> box;
	geom::envelope(geometry, box);

	uint id = indexedGeometries.size();
	if (isIndexed) {
		indices.at(layerName).insert(std::make_pair(box, id));
		if (hasName)
			indexedGeometryNames[id] = name;
	}

	uint tilex = 0, tiley = 0;
	switch(geomType) {
		case POINT_:
		{
			Point* p = boost::get<Point>(&geometry);
			if (p != nullptr) {
	
				Point sp(p->x()*10000000.0, p->y()*10000000.0);
				NodeID oid = store_point(sp);
				OutputObject oo(geomType, layerNum, oid, attrIdx, minzoom);
				if (isIndexed) indexedGeometries.push_back(oo);

				tilex =  lon2tilex(p->x(), baseZoom);
				tiley = latp2tiley(p->y(), baseZoom);
				AddObjectToTileIndex(TileCoordinates(tilex, tiley), oo);
			}
		} break;

		case LINESTRING_:
		{
			NodeID oid = store_linestring(boost::get<Linestring>(geometry));
			OutputObject oo(geomType, layerNum, oid, attrIdx, minzoom);
			if (isIndexed) indexedGeometries.push_back(oo);

			std::vector<OutputObject> oolist { oo };
			AddGeometryToIndex(boost::get<Linestring>(geometry), oolist);

		} break;

		case POLYGON_:
		{
			NodeID oid = store_multi_polygon(boost::get<MultiPolygon>(geometry));
			OutputObject oo(geomType, layerNum, oid, attrIdx, minzoom);
			if (isIndexed) indexedGeometries.push_back(oo);

			std::vector<OutputObject> oolist { oo };
			AddGeometryToIndex(boost::get<MultiPolygon>(geometry), oolist);
		} break;

		default:
			break;
	}
}
