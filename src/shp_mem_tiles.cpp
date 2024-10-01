#include "shp_mem_tiles.h"
#include "coordinates_geom.h"
#include <iostream>
#include <mutex>

using namespace std;
namespace geom = boost::geometry;
extern bool verbose;

ShpMemTiles::ShpMemTiles(size_t threadNum, uint indexZoom)
	: TileDataSource(threadNum, indexZoom, false)
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

	bitIndices[layerName] = std::vector<bool>();
	bitIndices[layerName].resize(2 * (1 << indexZoom) * (1 << indexZoom));
}

bool ShpMemTiles::mayIntersect(const std::string& layerName, const Box& box) const {
	// Check if any tiles in the bitmap might intersect this shape.
	// If none, downstream code can skip querying the r-tree.
	auto& bitvec = bitIndices.at(layerName);

	double lon1 = box.min_corner().x();
	double latp1 = box.min_corner().y();
	double lon2 = box.max_corner().x();
	double latp2 = box.max_corner().y();

	uint32_t x1 = lon2tilex(lon1, indexZoom);
	uint32_t x2 = lon2tilex(lon2, indexZoom);
	uint32_t y1 = latp2tiley(latp1, indexZoom);
	uint32_t y2 = latp2tiley(latp2, indexZoom);

	for (int x = std::min(x1, x2); x <= std::min((1u << indexZoom) - 1u, std::max(x1, x2)); x++) {
		for (int y = std::min(y1, y2); y <= std::min((1u << indexZoom) - 1u, std::max(y1, y2)); y++) {

			uint32_t index = 2 * (x * (1 << indexZoom) + y);
			if (bitvec[index]) {
				if (bitvec[index + 1])
					return true;
				else {
					// When we loaded the shapefiles, we did a rough index based on a bounding
					// box. For large, irregularly shaped polygons like national forests, this
					// can give false positives.
					//
					// We lazily do a more exacting check here, intersecting the index zoom tile.
					// Afterwards, we eitehr set bitvec[index + 1] or clear bitvec[index].
					TileBbox bbox(TileCoordinates(x, y), indexZoom, false, false);
					std::vector<uint> intersections = QueryMatchingGeometries(
						layerName,
						true,
						bbox.clippingBox,
						[&](const RTree &rtree) { // indexQuery
							vector<IndexValue> results;
							rtree.query(geom::index::intersects(bbox.clippingBox), back_inserter(results));
							return results;
						},
						[&](OutputObject const &oo) { // checkQuery
							return geom::intersects(bbox.clippingBox, retrieveMultiPolygon(oo.objectID));
						}
					);

					if (intersections.empty()) {
						bitvec[index] = false;
					} else {
						bitvec[index + 1] = true;
						return true;
					}
				}
			}
		}
	}

	return false;
}

void ShpMemTiles::StoreGeometry(
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

	uint tilex = 0, tiley = 0;
	std::shared_ptr<OutputObject> oo;
	switch(geomType) {
		case POINT_:
		{
			Point* p = boost::get<Point>(&geometry);
			if (p != nullptr) {
				Point sp(p->x()*10000000.0, p->y()*10000000.0);
				NodeID oid = storePoint(sp);
				oo = std::make_shared<OutputObject>(geomType, layerNum, oid, attrIdx, minzoom);
				tilex =  lon2tilex(p->x(), indexZoom);
				tiley = latp2tiley(p->y(), indexZoom);
				addObjectToSmallIndex(TileCoordinates(tilex, tiley), *oo, 0);
			} else { return; }
		} break;

		case LINESTRING_:
		{
			NodeID oid = storeLinestring(boost::get<Linestring>(geometry));
			oo = std::make_shared<OutputObject>(geomType, layerNum, oid, attrIdx, minzoom);
			std::vector<OutputObject> oolist { *oo };
			addGeometryToIndex(boost::get<Linestring>(geometry), oolist, 0);

		} break;

		case POLYGON_:
		{
			NodeID oid = storeMultiPolygon(boost::get<MultiPolygon>(geometry));
			oo = std::make_shared<OutputObject>(geomType, layerNum, oid, attrIdx, minzoom);
			std::vector<OutputObject> oolist { *oo };
			addGeometryToIndex(boost::get<MultiPolygon>(geometry), oolist, 0);

		} break;

		case MULTILINESTRING_:
		{
			NodeID oid = storeMultiLinestring(boost::get<MultiLinestring>(geometry));
			oo = std::make_shared<OutputObject>(geomType, layerNum, oid, attrIdx, minzoom);
			std::vector<OutputObject> oolist { *oo };
			addGeometryToIndex(boost::get<MultiLinestring>(geometry), oolist, 0);

		} break;

		default:
			throw std::runtime_error("Unknown geometry type");
	}

	// Add to index
	if (!isIndexed) return;
	std::lock_guard<std::mutex> indexLock(indexMutex);
	uint id = indexedGeometries.size();
	indices.at(layerName).insert(std::make_pair(box, id));
	if (hasName) { indexedGeometryNames[id] = name; }
	indexedGeometries.push_back(*oo);

	// Store a bitmap of which tiles at the indexZoom might intersect
	// this shape.
	auto& bitvec = bitIndices.at(layerName);
	double lon1 = box.min_corner().x();
	double latp1 = box.min_corner().y();
	double lon2 = box.max_corner().x();
	double latp2 = box.max_corner().y();

	uint32_t x1 = lon2tilex(lon1, indexZoom);
	uint32_t x2 = lon2tilex(lon2, indexZoom);
	uint32_t y1 = latp2tiley(latp1, indexZoom);
	uint32_t y2 = latp2tiley(latp2, indexZoom);

	uint32_t hits = 0;
	for (int x = std::min(x1, x2); x <= std::min((1u << indexZoom) - 1u, std::max(x1, x2)); x++) {
		for (int y = std::min(y1, y2); y <= std::min((1u << indexZoom) - 1u, std::max(y1, y2)); y++) {
			uint32_t index = 2 * (x * (1 << indexZoom) + y);
			if (!bitvec[index]) {
				hits++;
			}
			bitvec[index] = true;
		}
	}
}
