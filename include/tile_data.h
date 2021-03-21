/*! \file */ 
#ifndef _TILE_DATA_H
#define _TILE_DATA_H

#include <map>
#include <set>
#include <vector>
#include <memory>
#include "output_object.h"

typedef std::vector<OutputObjectRef>::const_iterator OutputObjectsConstIt;
typedef std::pair<OutputObjectsConstIt, OutputObjectsConstIt> OutputObjectsConstItPair;
typedef std::map<TileCoordinates, std::vector<OutputObjectRef>, TileCoordinatesCompare > TileIndex;
typedef std::set<TileCoordinates, TileCoordinatesCompare> TileCoordinatesSet;

class TileDataSource {

protected:	
	TileIndex tileIndex;
	unsigned int baseZoom;

public:
	TileDataSource(unsigned int baseZoom) 
		: baseZoom(baseZoom) 
	{ }

	///This must be thread safe!
	void MergeTileCoordsAtZoom(uint zoom, TileCoordinatesSet &dstCoords) {
		MergeTileCoordsAtZoom(zoom, baseZoom, tileIndex, dstCoords);
	}

	///This must be thread safe!
	void MergeSingleTileDataAtZoom(TileCoordinates dstIndex, uint zoom, std::vector<OutputObjectRef> &dstTile) {
		MergeSingleTileDataAtZoom(dstIndex, zoom, baseZoom, tileIndex, dstTile);
	}

	void AddObject(TileCoordinates const &index, OutputObjectRef const &oo) {
		tileIndex[index].push_back(oo);
	}

private:	
	static void MergeTileCoordsAtZoom(uint zoom, uint baseZoom, const TileIndex &srcTiles, TileCoordinatesSet &dstCoords);
	static void MergeSingleTileDataAtZoom(TileCoordinates dstIndex, uint zoom, uint baseZoom, const TileIndex &srcTiles, std::vector<OutputObjectRef> &dstTile);

};

static inline TileCoordinatesSet GetTileCoordinates(std::vector<class TileDataSource *> const &sources, unsigned int zoom) {
	TileCoordinatesSet tileCoordinates;

	// Create list of tiles
	tileCoordinates.clear();
	for(size_t i=0; i<sources.size(); i++)
		sources[i]->MergeTileCoordsAtZoom(zoom, tileCoordinates);

	return tileCoordinates;
}

static inline std::vector<OutputObjectRef> GetTileData(std::vector<class TileDataSource *> const &sources, TileCoordinates coordinates, unsigned int zoom)
{
	std::vector<OutputObjectRef> data;
	for(size_t i=0; i<sources.size(); i++)
		sources[i]->MergeSingleTileDataAtZoom(coordinates, zoom, data);

	sort(data.begin(), data.end());
	data.erase(unique(data.begin(), data.end()), data.end());
	return data;
}

static inline OutputObjectsConstItPair GetObjectsAtSubLayer(std::vector<OutputObjectRef> const &data, uint_least8_t layerNum) {
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

#endif //_TILE_DATA_H
