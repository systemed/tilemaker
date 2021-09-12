#include <algorithm>
#include <iostream>
#include "tile_data.h"

#include <ciso646>
#include <boost/sort/sort.hpp>

using namespace std;

typedef std::pair<OutputObjectsConstIt,OutputObjectsConstIt> OutputObjectsConstItPair;

void TileDataSource::MergeTileCoordsAtZoom(uint zoom, uint baseZoom, const TileCoordinatesSet &srcTiles, TileCoordinatesSet &dstCoords) {
	if (zoom==baseZoom) {
		// at z14, we can just use tileIndex
		for (auto it = srcTiles.begin(); it!= srcTiles.end(); ++it) {
			TileCoordinates index = *it;
			dstCoords.insert(index);
		}
	} else {
		// otherwise, we need to run through the z14 list, and assign each way
		// to a tile at our zoom level
		for (auto it = srcTiles.begin(); it!= srcTiles.end(); ++it) {
			TileCoordinates index = *it;
			TileCoordinate tilex = index.x / pow(2, baseZoom-zoom);
			TileCoordinate tiley = index.y / pow(2, baseZoom-zoom);
			TileCoordinates newIndex(tilex, tiley);
			dstCoords.insert(newIndex);
		}
	}
}

/*
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
} */

TileCoordinatesSet GetTileCoordinates(Box const &clippingBox, unsigned int baseZoom, unsigned int zoom) {
	TileCoordinatesSet srcCoords;

	auto p1 = clippingBox.min_corner();
	double min_x = lon2tilexf(p1.get<0>(), baseZoom);
	double max_y = latp2tileyf(p1.get<1>(), baseZoom);

	auto p2 = clippingBox.max_corner();
	double max_x = lon2tilexf(p2.get<0>(), baseZoom);
	double min_y = latp2tileyf(p2.get<1>(), baseZoom);

	for(double x = min_x; x < max_x; x += 1) {
		for(double y = min_y; y < max_y; y += 1) {
			TileCoordinate tileX1 = static_cast<TileCoordinate>(x);
			TileCoordinate tileY1 = static_cast<TileCoordinate>(y);
			srcCoords.insert(TileCoordinates(tileX1, tileY1));
		}
	}

	TileCoordinatesSet dstCoords;
	TileDataSource::MergeTileCoordsAtZoom(zoom, baseZoom, srcCoords, dstCoords);
	return dstCoords;
}

std::vector<OutputObjectRef> GetTileData(std::vector<class TileDataSource *> const &sources, TileCoordinates coordinates, unsigned int zoom)
{
	TileBbox bbox(coordinates, zoom);

	std::vector<OutputObjectRef> data;
	for(size_t i=0; i<sources.size(); i++)
		sources[i]->MergeSingleTileDataAtZoom(bbox.getTileBox(), data);

	boost::sort::pdqsort(data.begin(), data.end());
	data.erase(unique(data.begin(), data.end()), data.end());
	return data;
}

OutputObjectsConstItPair GetObjectsAtSubLayer(std::vector<OutputObjectRef> const &data, uint_least8_t layerNum) {
    struct layerComp
    {
        bool operator() ( const OutputObjectRef &x, uint_least8_t layer ) const { return x->layer < layer; }
        bool operator() ( uint_least8_t layer, const OutputObjectRef &x ) const { return layer < x->layer; }
    };

	// compare only by `layer`
	// We get the range within ooList, where the layer of each object is `layerNum`.
	// Note that ooList is sorted by a lexicographic order, `layer` being the most significant.
	return equal_range(data.begin(), data.end(), layerNum, layerComp());
}
