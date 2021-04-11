#include <algorithm>
#include <iostream>
#include "tile_data.h"
using namespace std;

typedef std::pair<OutputObjectsConstIt,OutputObjectsConstIt> OutputObjectsConstItPair;

void TileDataSource::MergeTileCoordsAtZoom(uint zoom, uint baseZoom, const TileIndex &srcTiles, TileCoordinatesSet &dstCoords) {
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

void TileDataSource::MergeSingleTileDataAtZoom(TileCoordinates dstIndex, uint zoom, uint baseZoom, const TileIndex &srcTiles, std::vector<OutputObjectRef> &dstTile) {
	if (zoom==baseZoom) {
		// at z14, we can just use tileIndex
		auto oosetIt = srcTiles.find(dstIndex);
		if(oosetIt == srcTiles.end()) return;
		dstTile.insert(dstTile.end(), oosetIt->second.begin(), oosetIt->second.end());
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
					dstTile.insert(dstTile.end(), oo);
				}
			}
		}
	}
}
