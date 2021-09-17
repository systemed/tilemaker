#include "shp_mem_tiles.h"
#include <iostream>
using namespace std;
namespace geom = boost::geometry;

ShpMemTiles::ShpMemTiles(OSMStore &osmStore, uint baseZoom)
	: TileDataSource(baseZoom), osmStore(osmStore)
{ }

// Look for shapefile objects that fulfil a spatial query (e.g. intersects)
// Parameters:
// - shapefile layer name to search
// - bounding box to match against
// - indexQuery(rtree, results) lambda, implements: rtree.query(geom::index::covered_by(box), back_inserter(results))
// - checkQuery(osmstore, id) lambda, implements:   return geom::covered_by(osmStore.retrieve(id), geom)
vector<uint> ShpMemTiles::QueryMatchingGeometries(const string &layerName, bool once, Box &box, 
	function<vector<IndexValue>(const RTree &rtree)> indexQuery, 
	function<bool(OutputObject const &oo)> checkQuery) const {
	
	// Find the layer
	auto f = indices.find(layerName); // f is an RTree
	if (f==indices.end()) {
		cerr << "Couldn't find indexed layer " << layerName << endl;
		return vector<uint>();	// empty, relations not supported
	}
	
	// Run the index query
	vector<IndexValue> results = indexQuery(f->second);
	
	// Run the check query
	vector<uint> ids;
	for (auto it: results) {
		uint id = it.second;
		if (checkQuery(*cachedGeometries.at(id))) { ids.push_back(id); if (once) break; }
	}
	return ids;
}

vector<string> ShpMemTiles::namesOfGeometries(const vector<uint> &ids) const {
	vector<string> names;
	for (uint i=0; i<ids.size(); i++) {
		if (cachedGeometryNames.find(ids[i])!=cachedGeometryNames.end()) {
			names.push_back(cachedGeometryNames.at(ids[i]));
		}
	}
	return names;
}

void ShpMemTiles::CreateNamedLayerIndex(const std::string &layerName) {
	indices[layerName]=RTree();
}

OutputObjectRef ShpMemTiles::AddObject(uint_least8_t layerNum,
	const std::string &layerName, enum OutputGeometryType geomType,
	Geometry geometry, bool isIndexed, bool hasName, const std::string &name, AttributeStoreRef attributes) {		

	geom::model::box<Point> box;
	geom::envelope(geometry, box);

	uint id = cachedGeometries.size();
	if (isIndexed) {
		indices.at(layerName).insert(std::make_pair(box, id));
		if (hasName) cachedGeometryNames[id]=name;
	}

	OutputObjectRef oo;

	uint tilex = 0, tiley = 0;
	switch(geomType) {
		case POINT_:
		{
			Point *p = boost::get<Point>(&geometry);
			if (p != nullptr) {
	
				osmStore.store_point(osmStore.shp(), id, *p);
				oo = CreateObject(*p, OutputObjectOsmStorePoint(
					geomType, layerNum, id, attributes));
				cachedGeometries.push_back(oo);
			}
		} break;

		case LINESTRING_:
		{
			osmStore.store_linestring(osmStore.shp(), id, boost::get<Linestring>(geometry));
			oo = CreateObject(getEnvelope(boost::get<Linestring>(geometry)), OutputObjectOsmStoreLinestring(
						geomType, layerNum, id, attributes));
			if(oo) cachedGeometries.push_back(oo);
		} break;

		case POLYGON_:
		{
			osmStore.store_multi_polygon(osmStore.shp(), id, boost::get<MultiPolygon>(geometry));
			oo = CreateObject(getEnvelope(boost::get<MultiPolygon>(geometry)), OutputObjectOsmStoreMultiPolygon(
						geomType, layerNum, id, attributes));
			if(oo) cachedGeometries.push_back(oo);
		} break;

		default:
			break;
	}

	return oo;
}


