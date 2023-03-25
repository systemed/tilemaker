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
typedef std::map<TileCoordinates, std::vector<OutputObjectRef>, TileCoordinatesCompare> TileIndex;
typedef std::set<TileCoordinates, TileCoordinatesCompare> TileCoordinatesSet;

class TileDataSource {

protected:	
	std::mutex mutex;
	TileIndex tileIndex;
	std::deque<OutputObject> objects;
	
	// rtree index of large objects
	using oo_rtree_param_type = boost::geometry::index::quadratic<128>;
	boost::geometry::index::rtree< std::pair<Box,OutputObjectRef>, oo_rtree_param_type> box_rtree;

	unsigned int baseZoom;

public:
	TileDataSource(unsigned int baseZoom) 
		: baseZoom(baseZoom) 
	{ }

	///This must be thread safe!
	void MergeTileCoordsAtZoom(uint zoom, TileCoordinatesSet &dstCoords) {
		MergeTileCoordsAtZoom(zoom, baseZoom, tileIndex, dstCoords);
	}

	void MergeLargeCoordsAtZoom(uint zoom, TileCoordinatesSet &dstCoords);

	///This must be thread safe!
	void MergeSingleTileDataAtZoom(TileCoordinates dstIndex, uint zoom, std::vector<OutputObjectRef> &dstTile) {
		MergeSingleTileDataAtZoom(dstIndex, zoom, baseZoom, tileIndex, dstTile);
	}

	OutputObjectRef CreateObject(OutputObject const &oo) {
		std::lock_guard<std::mutex> lock(mutex);
		objects.push_back(oo);
		return &objects.back();
	}

	void AddObject(TileCoordinates const &index, OutputObjectRef const &oo) {
		std::lock_guard<std::mutex> lock(mutex);
		tileIndex[index].push_back(oo);
	}

	void AddObjectToLargeIndex(Box const &envelope, OutputObjectRef const &oo) {
		std::lock_guard<std::mutex> lock(mutex);
		box_rtree.insert(std::make_pair(envelope, oo));
	}

	void MergeLargeObjects(TileCoordinates dstIndex, uint zoom, std::vector<OutputObjectRef> &dstTile);

private:	
	static void MergeTileCoordsAtZoom(uint zoom, uint baseZoom, const TileIndex &srcTiles, TileCoordinatesSet &dstCoords);
	static void MergeSingleTileDataAtZoom(TileCoordinates dstIndex, uint zoom, uint baseZoom, const TileIndex &srcTiles, std::vector<OutputObjectRef> &dstTile);
};

TileCoordinatesSet GetTileCoordinates(std::vector<class TileDataSource *> const &sources, unsigned int zoom);

std::vector<OutputObjectRef> GetTileData(std::vector<class TileDataSource *> const &sources, TileCoordinates coordinates, unsigned int zoom);

OutputObjectsConstItPair GetObjectsAtSubLayer(std::vector<OutputObjectRef> const &data, uint_least8_t layerNum);

#endif //_TILE_DATA_H
