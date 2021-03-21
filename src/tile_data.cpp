#include <algorithm>
#include <iostream>
#include "tile_data.h"
using namespace std;

typedef std::pair<OutputObjectsConstIt,OutputObjectsConstIt> OutputObjectsConstItPair;

void MergeTileCoordsAtZoom(uint zoom, uint baseZoom, const TileIndex &srcTiles, TileCoordinatesSet &dstCoords) {
	if (zoom==baseZoom) {
		// at z14, we can just use tileIndex
		for (auto it = srcTiles.begin(); it!= srcTiles.end(); ++it) {
			TileCoordinates index = it->first;
			dstCoords.insert(index);
		}
	} else {
		// otherwise, we need to run through the z14 list, and assign each way
		// to a tile at our zoom level
		for (auto it = srcTiles.begin(); it!= srcTiles.end(); ++it) {
			TileCoordinates index = it->first;
			TileCoordinate tilex = index.x / pow(2, baseZoom-zoom);
			TileCoordinate tiley = index.y / pow(2, baseZoom-zoom);
			TileCoordinates newIndex(tilex, tiley);
			dstCoords.insert(newIndex);
		}
	}
}

void MergeSingleTileDataAtZoom(TileCoordinates dstIndex, uint zoom, uint baseZoom, const TileIndex &srcTiles, OutputObjectsPerLayer &dstTile) {
	if (zoom==baseZoom) {
		// at z14, we can just use tileIndex
		auto oosetIt = srcTiles.find(dstIndex);
		if(oosetIt == srcTiles.end()) return;
		for (auto it = oosetIt->second.begin(); it != oosetIt->second.end(); ++it) {
			OutputObjectRef oo = *it;
			dstTile.resize(std::max<std::size_t>(dstTile.size(), oo->layer + 1));	
			dstTile[oo->layer].insert(oo);
		}
	} else {
		// otherwise, we need to run through the z14 list, and assign each way
		// to a tile at our zoom level
		int scale = pow(2, baseZoom-zoom);
		TileCoordinates srcIndex1(dstIndex.x*scale, dstIndex.y*scale);
		TileCoordinates srcIndex2((dstIndex.x+1)*scale, (dstIndex.y+1)*scale);

		for(int x=srcIndex1.x; x<srcIndex2.x; x++)
		{
			for(int y=srcIndex1.y; y<srcIndex2.y; y++)
			{
				TileCoordinates srcIndex(x, y);
				auto oosetIt = srcTiles.find(srcIndex);
				if(oosetIt == srcTiles.end()) continue;
				for (auto it = oosetIt->second.begin(); it != oosetIt->second.end(); ++it) {
					OutputObjectRef oo = *it;
					if (oo->minZoom > zoom) continue;
					dstTile.resize(std::max<std::size_t>(dstTile.size(), oo->layer + 1));	
					dstTile[oo->layer].insert(oo);
				}
			}
		}
	}
}

// ********************************

TilesAtZoomIterator::TilesAtZoomIterator(TileCoordinatesSet::const_iterator it, class TileData &tileData, uint zoom):
	TileCoordinatesSet::const_iterator(it),
	tileData(tileData),
	zoom(zoom) {

	RefreshData();
}

TileCoordinates TilesAtZoomIterator::GetCoordinates() const {
	TileCoordinatesSet::const_iterator it = *this;
	return *it;
}

TilesAtZoomIterator& TilesAtZoomIterator::operator++() {
	TileCoordinatesSet::const_iterator::operator++();
	RefreshData();
	return *this;
}

TilesAtZoomIterator TilesAtZoomIterator::operator++(int a) {
	TileCoordinatesSet::const_iterator::operator++(a);
	RefreshData();
	return *this;
}

TilesAtZoomIterator& TilesAtZoomIterator::operator--() {
	TileCoordinatesSet::const_iterator::operator--();
	RefreshData();
	return *this;
}

TilesAtZoomIterator TilesAtZoomIterator::operator--(int a) {
	TileCoordinatesSet::const_iterator::operator--(a);
	RefreshData();
	return *this;
}

void TilesAtZoomIterator::RefreshData() {
	data.clear();
	data.reserve(reserve_output_objects);

	TileCoordinatesSet::const_iterator it = *this;
	if(it == tileData.tileCoordinates.end()) return;

	for(size_t i=0; i<tileData.sources.size(); i++)
		tileData.sources[i]->MergeSingleTileDataAtZoom(*it, zoom, data);
}

// *********************************

TileData::TileData(std::vector<class TileDataSource *> const &sources, uint zoom):
	sources(sources),
	zoom(zoom) {

	// Create list of tiles
	tileCoordinates.clear();
	for(size_t i=0; i<sources.size(); i++)
		sources[i]->MergeTileCoordsAtZoom(zoom, tileCoordinates);
}

class TilesAtZoomIterator TileData::GetTilesAtZoomBegin() {
	return TilesAtZoomIterator(tileCoordinates.begin(), *this, zoom);
}

class TilesAtZoomIterator TileData::GetTilesAtZoomEnd() {
	return TilesAtZoomIterator(tileCoordinates.end(), *this, zoom);
}

size_t TileData::GetTilesAtZoomSize() const {
	return tileCoordinates.size();
}


