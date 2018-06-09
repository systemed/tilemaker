#include "shp_mem_tiles.h"
using namespace std;

ShpMemTiles::ShpMemTiles(uint baseZoom):
	TileDataSource(),
	baseZoom(baseZoom)
{

}

void ShpMemTiles::MergeTileCoordsAtZoom(uint zoom, TileCoordinatesSet &dstCoords)
{
	::MergeTileCoordsAtZoom(zoom, baseZoom, tileIndex, dstCoords);
}

void ShpMemTiles::MergeSingleTileDataAtZoom(TileCoordinates dstIndex, uint zoom, 
	std::vector<OutputObjectRef> &dstTile)
{
	::MergeSingleTileDataAtZoom(dstIndex, zoom, baseZoom, tileIndex, dstTile);
}

// Find intersecting shapefile layer
// TODO: multipolygon relations not supported, will always return false
vector<string> ShpMemTiles::FindIntersecting(const string &layerName, Box &box) {
	vector<uint> ids = findIntersectingGeometries(layerName, box);
	return namesOfGeometries(ids);
}

bool ShpMemTiles::Intersects(const string &layerName, Box &box) {
	return !findIntersectingGeometries(layerName, box).empty();
}

vector<uint> ShpMemTiles::findIntersectingGeometries(const string &layerName, Box &box) {
	vector<IndexValue> results;
	vector<uint> ids;

	auto f = indices.find(layerName);
	if (f==indices.end()) {
		cerr << "Couldn't find indexed layer " << layerName << endl;
		return vector<uint>();	// empty, relations not supported
	}

	f->second.query(geom::index::intersects(box), back_inserter(results));
	return verifyIntersectResults(results,box.min_corner(),box.max_corner());
}

vector<uint> ShpMemTiles::verifyIntersectResults(vector<IndexValue> &results, Point &p1, Point &p2) {
	vector<uint> ids;
	for (auto it : results) {
		uint id=it.second;
		if      (geom::intersects(cachedGeometries.at(id),p1)) { ids.push_back(id); }
		else if (geom::intersects(cachedGeometries.at(id),p2)) { ids.push_back(id); }
	}
	return ids;
}

vector<string> ShpMemTiles::namesOfGeometries(vector<uint> &ids) {
	vector<string> names;
	for (uint i=0; i<ids.size(); i++) {
		if (cachedGeometryNames.find(ids[i])!=cachedGeometryNames.end()) {
			names.push_back(cachedGeometryNames.at(ids[i]));
		}
	}
	return names;
}

void ShpMemTiles::CreateNamedLayerIndex(const std::string &layerName)
{
	indices[layerName]=RTree();
}

void ShpMemTiles::AddObjectedToIndex(const std::string &layerName, Box &box, bool hasName, const std::string &name)
{
	uint id = cachedGeometries.size()-1;
	indices.at(layerName).insert(std::make_pair(box, id));
	if(hasName)
		cachedGeometryNames[id]=name;
}

