#include "osm_mem_tiles.h"
#include "node_store.h"
#include "way_store.h"
using namespace std;

thread_local GeometryCache<Linestring> linestringCache;

OsmMemTiles::OsmMemTiles(
	size_t threadNum,
	uint indexZoom,
	bool includeID,
	const NodeStore& nodeStore,
	const WayStore& wayStore
)
	: TileDataSource(threadNum, indexZoom, includeID),
	nodeStore(nodeStore),
	wayStore(wayStore)
{
}

LatpLon OsmMemTiles::buildNodeGeometry(
	NodeID const objectID,
	const TileBbox &bbox
) const {
	if (objectID < OSM_THRESHOLD) {
		return TileDataSource::buildNodeGeometry(objectID, bbox);
	}

	if (IS_NODE(objectID))
		return nodeStore.at(OSM_ID(objectID));


	if (IS_WAY(objectID)) {
		Linestring& ls = getOrBuildLinestring(objectID);
		Point centroid;
		Polygon p;
		geom::assign_points(p, ls);
		geom::centroid(p, centroid);
		return LatpLon{(int32_t)(centroid.y()*10000000.0), (int32_t)(centroid.x()*10000000.0)};
	}

	throw std::runtime_error("OsmMemTiles::buildNodeGeometry: unsupported objectID");
}

Geometry OsmMemTiles::buildWayGeometry(
	const OutputGeometryType geomType, 
	const NodeID objectID,
	const TileBbox &bbox
) {
	if (objectID < OSM_THRESHOLD || (geomType == POLYGON_ && IS_WAY(objectID))) {
		return TileDataSource::buildWayGeometry(geomType, objectID, bbox);
	}

	if (geomType == LINESTRING_ && IS_WAY(objectID)) {
		Linestring& ls = getOrBuildLinestring(objectID);

		MultiLinestring out;
		if(ls.empty())
			return out;

		Linestring current_ls;
		geom::append(current_ls, ls[0]);

		for(size_t i = 1; i < ls.size(); ++i) {
			if(!geom::intersects(Linestring({ ls[i-1], ls[i] }), bbox.clippingBox)) {
				if(current_ls.size() > 1)
					out.push_back(std::move(current_ls));
				current_ls.clear();
			}
			geom::append(current_ls, ls[i]);
		}

		if(current_ls.size() > 1)
			out.push_back(std::move(current_ls));

		MultiLinestring result;
		geom::intersection(out, bbox.getExtendBox(), result);
		return result;

	}

	throw std::runtime_error("buildWayGeometry: unexpected objectID: " + std::to_string(objectID));
}

void OsmMemTiles::populateLinestring(Linestring& ls, NodeID objectID) const {
	std::vector<LatpLon> nodes = wayStore.at(OSM_ID(objectID));

	for (const LatpLon& node : nodes) {
		boost::geometry::range::push_back(ls, boost::geometry::make<Point>(node.lon/10000000.0, node.latp/10000000.0));
	}
}

Linestring& OsmMemTiles::getOrBuildLinestring(NodeID objectID) const {
	// Note: this function returns a reference, not a shared_ptr.
	//
	// This is safe, because this function is the only thing that can
	// create/destroy entries in the cache, and the cache is thread-local.

	Linestring* cachedEntry = linestringCache.get(objectID);
	if (cachedEntry != nullptr)
		return *cachedEntry;

	std::shared_ptr<Linestring> rv = std::make_shared<Linestring>();

	Linestring& ls = *rv;
	populateLinestring(ls, objectID);

	linestringCache.add(objectID, rv);
	return *rv;
}

void OsmMemTiles::populateMultiPolygon(MultiPolygon& dst, NodeID objectID) {
	if (objectID < OSM_THRESHOLD)
		return TileDataSource::populateMultiPolygon(dst, objectID);

	// We don't cache the linestrings used for polygons. Polygons from ways seem
	// to be dominated by polygons with few points that only span a basezoom tile,
	// e.g. building outlines.
	//
	// There are some exceptions, but some rough measurements showed caching
	// hurt overall throughput.
	Linestring ls;
	populateLinestring(ls, objectID);
	Polygon p;
	geom::assign_points(p, ls);
	dst.push_back(p);
}

void OsmMemTiles::Clear() {
	for (auto& entry : objects)
		entry.clear();
	for (auto& entry : objectsWithIds)
		entry.clear();
}
