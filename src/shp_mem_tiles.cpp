#include "shp_mem_tiles.h"
#include <iostream>
using namespace std;
namespace geom = boost::geometry;
#include "read_shp.h"

ShpMemTiles::ShpMemTiles(uint baseZoom):
	TileDataSource(),
	baseZoom(baseZoom)
{

}

void ShpMemTiles::GenerateTileListAtZoom(uint zoom, TileCoordinatesSet &dstCoords)
{
	::GenerateTileListFromTileIndex(zoom, baseZoom, tileIndex, dstCoords);
}

void ShpMemTiles::GetTileData(TileCoordinates dstIndex, uint zoom, 
	std::vector<OutputObjectRef> &dstTile)
{
	::GetTileDataFromTileIndex(dstIndex, zoom, baseZoom, tileIndex, dstTile);
}

// Find intersecting shapefile layer
// TODO: multipolygon relations not supported, will always return false
vector<string> ShpMemTiles::FindIntersecting(const string &layerName, Box &box) const {
	vector<uint> ids = findIntersectingGeometries(layerName, box);
	return namesOfGeometries(ids);
}

bool ShpMemTiles::Intersects(const string &layerName, Box &box) const {
	return !findIntersectingGeometries(layerName, box).empty();
}

vector<uint> ShpMemTiles::findIntersectingGeometries(const string &layerName, Box &box) const {
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

vector<uint> ShpMemTiles::verifyIntersectResults(vector<IndexValue> &results, Point &p1, Point &p2) const {
	vector<uint> ids;
	for (auto it : results) {
		uint id=it.second;
		if      (geom::intersects(cachedGeometries.at(id),p1)) { ids.push_back(id); }
		else if (geom::intersects(cachedGeometries.at(id),p2)) { ids.push_back(id); }
	}
	return ids;
}

vector<string> ShpMemTiles::namesOfGeometries(vector<uint> &ids) const {
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

OutputObjectRef ShpMemTiles::AddObject(uint_least8_t layerNum,
	const std::string &layerName, enum OutputGeometryType geomType,
	Geometry geometry, bool isIndexed, bool hasName, const std::string &name)
{		
	geom::model::box<Point> box;
	geom::envelope(geometry, box);

	cachedGeometries.push_back(geometry);

	uint id = cachedGeometries.size()-1;
	if(isIndexed)
	{
		indices.at(layerName).insert(std::make_pair(box, id));
		if(hasName)
			cachedGeometryNames[id]=name;
	}

	OutputObjectRef oo = std::make_shared<OutputObjectCached>(geomType, layerNum, cachedGeometries.size()-1, cachedGeometries);

	Point *p = nullptr;
	uint tilex = 0, tiley = 0;
	switch(geomType)
	{
	case CACHED_POINT:
		p = boost::get<Point>(&geometry);
		if(p!=nullptr)
		{
			tilex =  lon2tilex(p->x(), baseZoom);
			tiley = latp2tiley(p->y(), baseZoom);
			tileIndex[TileCoordinates(tilex, tiley)].push_back(oo);
		}
		break;
	case CACHED_LINESTRING:
		addToTileIndexPolyline(oo, tileIndex, &geometry);
		break;
	case CACHED_POLYGON:
		// add to tile index
		addToTileIndexByBbox(oo, tileIndex, 
			box.min_corner().get<0>(), box.min_corner().get<1>(), 
			box.max_corner().get<0>(), box.max_corner().get<1>());
		break;
	default:
		break;
	}

	return oo;
}

uint ShpMemTiles::GetBaseZoom()
{
	return baseZoom;
}

void ShpMemTiles::Load(class LayerDefinition &layers, 
	bool hasClippingBox,
	const Box &clippingBox)
{
	for(size_t layerNum=0; layerNum<layers.layers.size(); layerNum++)	
	{
		// External layer sources
		const LayerDef &layer = layers.layers[layerNum];
		if(layer.indexed)
			this->CreateNamedLayerIndex(layer.name);

		if (layer.source.size()>0) {
			if (!hasClippingBox) {
				cerr << "Can't read shapefiles unless a bounding box is provided." << endl;
				exit(EXIT_FAILURE);
			}
			Box projClippingBox = Box(geom::make<Point>(clippingBox.min_corner().get<0>(), lat2latp(clippingBox.min_corner().get<1>())),
			              geom::make<Point>(clippingBox.max_corner().get<0>(), lat2latp(clippingBox.max_corner().get<1>())));

			readShapefile(projClippingBox,
			              layers,
			              baseZoom, layerNum,
						  *this);
		}
	}
}

// Add an OutputObject to all tiles between min/max lat/lon
void ShpMemTiles::addToTileIndexByBbox(OutputObjectRef &oo, TileIndex &tileIndex,
                          double minLon, double minLatp, double maxLon, double maxLatp) {
	uint minTileX =  lon2tilex(minLon, baseZoom);
	uint maxTileX =  lon2tilex(maxLon, baseZoom);
	uint minTileY = latp2tiley(minLatp, baseZoom);
	uint maxTileY = latp2tiley(maxLatp, baseZoom);
	for (uint x=min(minTileX,maxTileX); x<=max(minTileX,maxTileX); x++) {
		for (uint y=min(minTileY,maxTileY); y<=max(minTileY,maxTileY); y++) {
			TileCoordinates index(x, y);
			tileIndex[index].push_back(oo);
		}
	}
}

// Add an OutputObject to all tiles along a polyline
void ShpMemTiles::addToTileIndexPolyline(OutputObjectRef &oo, TileIndex &tileIndex, Geometry *geom) {

	const Linestring *ls = boost::get<Linestring>(geom);
	if(ls == nullptr) return;
	uint lastx = UINT_MAX;
	uint lasty;
	for (Linestring::const_iterator jt = ls->begin(); jt != ls->end(); ++jt) {
		uint tilex =  lon2tilex(jt->get<0>(), baseZoom);
		uint tiley = latp2tiley(jt->get<1>(), baseZoom);
		if (lastx==UINT_MAX) {
			tileIndex[TileCoordinates(tilex, tiley)].push_back(oo);
		} else if (lastx!=tilex || lasty!=tiley) {
			for (uint x=min(tilex,lastx); x<=max(tilex,lastx); x++) {
				for (uint y=min(tiley,lasty); y<=max(tiley,lasty); y++) {
					tileIndex[TileCoordinates(x, y)].push_back(oo);
				}
			}
		}
		lastx=tilex; lasty=tiley;
	}
}

