#include <algorithm>
#include <iostream>
#include "tile_data.h"
using namespace std;

typedef std::pair<OutputObjectsConstIt,OutputObjectsConstIt> OutputObjectsConstItPair;

TileIndex::TileIndex(uint baseZoom):
	baseZoom(baseZoom)
{

}

TileIndex::~TileIndex()
{

}

void TileIndex::GenerateTileList(uint destZoom, TileCoordinatesSet &dstCoords) const
{
	if (destZoom==this->baseZoom) {
		// at z14, we can just use tileIndex
		for (auto it = this->index.begin(); it!= this->index.end(); ++it) {
			TileCoordinates index = it->first;
			dstCoords.insert(index);
		}
	} else {
		// otherwise, we need to run through the z14 list, and assign each way
		// to a tile at our zoom level
		if(destZoom < this->baseZoom)
		{
			int scale = pow(2, this->baseZoom-destZoom);
			for (auto it = this->index.begin(); it!= this->index.end(); ++it) {
				TileCoordinates index = it->first;
				TileCoordinate tilex = index.x / scale;
				TileCoordinate tiley = index.y / scale;
				TileCoordinates newIndex(tilex, tiley);
				dstCoords.insert(newIndex);
			}
		}
		else
		{
			throw runtime_error("Not implemented");
		}
	}
}

void TileIndex::GetTileData(TileCoordinates dstIndex, uint destZoom,
	std::vector<OutputObjectRef> &dstTile) const
{
	if (destZoom==this->baseZoom) {
		// at z14, we can just use tileIndex
		auto oosetIt = this->index.find(dstIndex);
		if(oosetIt == this->index.end()) return;
		dstTile.insert(dstTile.end(), oosetIt->second.begin(), oosetIt->second.end());
	} else {
		// otherwise, we need to run through the z14 list, and assign each way
		// to a tile at our zoom level
		if(destZoom < this->baseZoom)
		{
			int scale = pow(2, this->baseZoom-destZoom);
			TileCoordinates srcIndex1(dstIndex.x*scale, dstIndex.y*scale);
			TileCoordinates srcIndex2((dstIndex.x+1)*scale, (dstIndex.y+1)*scale);

			for(int x=srcIndex1.x; x<srcIndex2.x; x++)
			{
				for(int y=srcIndex1.y; y<srcIndex2.y; y++)
				{
					TileCoordinates srcIndex(x, y);
					auto oosetIt = this->index.find(srcIndex);
					if(oosetIt == this->index.end()) continue;
					dstTile.insert(dstTile.end(), oosetIt->second.begin(), oosetIt->second.end());
					//cout << oosetIt->second.size() << endl;
				}
			}
		}
		else
		{
			int scale = pow(2, destZoom-this->baseZoom);
			TileCoordinates srcIndex(dstIndex.x/scale, dstIndex.y/scale);
			auto oosetIt = this->index.find(srcIndex);
			if(oosetIt == this->index.end()) return;
			dstTile.insert(dstTile.end(), oosetIt->second.begin(), oosetIt->second.end());
		}
	}
}

uint TileIndex::GetBaseZoom() const
{
	return this->baseZoom;
}

void TileIndex::Add(TileCoordinates tileIndex, OutputObjectRef oo)
{
	this->index[tileIndex].push_back(oo);
}

void TileIndex::Add(OutputObjectRef &oo, Point pt)
{
	uint tilex = 0, tiley = 0;
	tilex = lon2tilex(pt.x(), baseZoom);
	tiley = latp2tiley(pt.y(), baseZoom);
	this->index[TileCoordinates(tilex, tiley)].push_back(oo);
}

// Add an OutputObject to all tiles between min/max lat/lon
void TileIndex::AddByBbox(OutputObjectRef &oo,
                          double minLon, double minLatp, double maxLon, double maxLatp) {
	uint minTileX =  lon2tilex(minLon, baseZoom);
	uint maxTileX =  lon2tilex(maxLon, baseZoom);
	uint minTileY = latp2tiley(minLatp, baseZoom);
	uint maxTileY = latp2tiley(maxLatp, baseZoom);
	for (uint x=min(minTileX,maxTileX); x<=max(minTileX,maxTileX); x++) {
		for (uint y=min(minTileY,maxTileY); y<=max(minTileY,maxTileY); y++) {
			TileCoordinates index(x, y);
			this->index[index].push_back(oo);
		}
	}
}

// Add an OutputObject to all tiles along a polyline
void TileIndex::AddByPolyline(OutputObjectRef &oo, Geometry *geom) {

	const Linestring *ls = boost::get<Linestring>(geom);
	if(ls == nullptr) return;
	uint lastx = UINT_MAX;
	uint lasty;
	for (Linestring::const_iterator jt = ls->begin(); jt != ls->end(); ++jt) {
		uint tilex =  lon2tilex(jt->get<0>(), baseZoom);
		uint tiley = latp2tiley(jt->get<1>(), baseZoom);
		if (lastx==UINT_MAX) {
			this->index[TileCoordinates(tilex, tiley)].push_back(oo);
		} else if (lastx!=tilex || lasty!=tiley) {
			for (uint x=min(tilex,lastx); x<=max(tilex,lastx); x++) {
				for (uint y=min(tiley,lasty); y<=max(tiley,lasty); y++) {
					this->index[TileCoordinates(x, y)].push_back(oo);
				}
			}
		}
		lastx=tilex; lasty=tiley;
	}
}

// *********************************

ObjectsAtSubLayerIterator::ObjectsAtSubLayerIterator(OutputObjectsConstIt it, const class TileData &tileData):
	OutputObjectsConstIt(it),
	tileData(tileData)
{

}

// ********************************

TilesAtZoomIterator::TilesAtZoomIterator(TileCoordinatesSet::const_iterator it, class TileData &tileData, uint zoom):
	TileCoordinatesSet::const_iterator(it),
	tileData(tileData),
	zoom(zoom)
{
	ready = false;
}

TileCoordinates TilesAtZoomIterator::GetCoordinates() const
{
	TileCoordinatesSet::const_iterator it = *this;
	return *it;
}

ObjectsAtSubLayerConstItPair TilesAtZoomIterator::GetObjectsAtSubLayer(uint_least8_t layerNum)
{
	if(!ready)
		RefreshData();

	// compare only by `layer`
	auto layerComp = [](const OutputObjectRef &x, const OutputObjectRef &y) -> bool { return x->layer < y->layer; };
	// We get the range within ooList, where the layer of each object is `layerNum`.
	// Note that ooList is sorted by a lexicographic order, `layer` being the most significant.
	const std::vector<OutputObjectRef> &ooList = data;
	Geometry geom;
	OutputObjectRef referenceObj = make_shared<OutputObjectOsmStore>(POINT, layerNum, 0, geom);
	OutputObjectsConstItPair ooListSameLayer = equal_range(ooList.begin(), ooList.end(), referenceObj, layerComp);
	return ObjectsAtSubLayerConstItPair(ObjectsAtSubLayerIterator(ooListSameLayer.first, tileData), ObjectsAtSubLayerIterator(ooListSameLayer.second, tileData));
}

TilesAtZoomIterator& TilesAtZoomIterator::operator++()
{
	TileCoordinatesSet::const_iterator::operator++();
	ready = false;
	data.clear();
	return *this;
}

TilesAtZoomIterator TilesAtZoomIterator::operator++(int a)
{
	TileCoordinatesSet::const_iterator::operator++(a);
	ready = false;
	data.clear();
	return *this;
}

TilesAtZoomIterator& TilesAtZoomIterator::operator--()
{
	TileCoordinatesSet::const_iterator::operator--();
	ready = false;
	data.clear();
	return *this;
}

TilesAtZoomIterator TilesAtZoomIterator::operator--(int a)
{
	TileCoordinatesSet::const_iterator::operator--(a);
	ready = false;
	data.clear();
	return *this;
}

void TilesAtZoomIterator::RefreshData()
{
	data.clear();
	TileCoordinatesSet::const_iterator it = *this;
	if(it == tileData.tileCoordinates.end()) return;

	for(size_t i=0; i<tileData.sources.size(); i++)
		tileData.sources[i]->GetTileData(*it, zoom, data);

	sort(data.begin(), data.end());
	data.erase(unique(data.begin(), data.end()), data.end());
	ready = true;
}

// *********************************

TileData::TileData(const std::vector<class TileDataSource *> sources):
	sources(sources)
{
	zoom = 0;
}

class TilesAtZoomIterator TileData::GetTilesAtZoomBegin()
{
	return TilesAtZoomIterator(tileCoordinates.begin(), *this, zoom);
}

class TilesAtZoomIterator TileData::GetTilesAtZoomEnd()
{
	return TilesAtZoomIterator(tileCoordinates.end(), *this, zoom);
}

size_t TileData::GetTilesAtZoomSize()
{
	size_t count=0;
	for(auto it = tileCoordinates.begin(); it != tileCoordinates.end(); it++) count++;
	return count;
}

void TileData::SetZoom(uint zoom)
{
	this->zoom = zoom;
	// Create list of tiles
	tileCoordinates.clear();
	for(size_t i=0; i<sources.size(); i++)
		sources[i]->GenerateTileListAtZoom(zoom, tileCoordinates);
}

