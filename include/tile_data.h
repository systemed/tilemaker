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

/** 
 * \brief Get the envelope of this geometry
 */
template<class Geometry>
static inline Box getEnvelope(Geometry const &g)
{
	Box result;
	boost::geometry::envelope(g, result);
	return result;
}

class TileDataSource {

protected:	
	std::mutex mutex;
	//TileIndex tileIndex;
	std::deque<OutputObject> objects;

	using oo_rtree_param_type = boost::geometry::index::quadratic<16>;
	boost::geometry::index::rtree< std::pair<Point, OutputObjectRef>, oo_rtree_param_type> point_rtree;
	boost::geometry::index::rtree< std::pair<Box, OutputObjectRef>, oo_rtree_param_type> box_rtree;

	unsigned int baseZoom;

public:
	TileDataSource(unsigned int baseZoom) 
		: baseZoom(baseZoom) 
	{ }

	///This must be thread safe!
	void MergeSingleTileDataAtZoom(Box const &box, std::vector<OutputObjectRef> &dstTile) {
   		for(auto const &result: point_rtree | boost::geometry::index::adaptors::queried(boost::geometry::index::intersects(box)))
			dstTile.push_back(result.second);
   		for(auto const &result: box_rtree | boost::geometry::index::adaptors::queried(boost::geometry::index::intersects(box)))
			dstTile.push_back(result.second);
//		MergeSingleTileDataAtZoom(dstIndex, zoom, baseZoom, tileIndex, dstTile);
	}

	OutputObjectRef CreateObject(Box const &envelope, OutputObject const &oo) {
		if(!boost::geometry::is_valid(envelope))
			return OutputObjectRef();

		std::lock_guard<std::mutex> lock(mutex);
		objects.push_back(oo);

		box_rtree.insert(std::make_pair(envelope, &objects.back()));
		return &objects.back();
	}

	OutputObjectRef CreateObject(Point const &p, OutputObject const &oo) {
		std::lock_guard<std::mutex> lock(mutex);
		objects.push_back(oo);

		point_rtree.insert(std::make_pair(p, &objects.back()));
		return &objects.back();
	}

	/* void AddObject(TileCoordinates const &index, OutputObjectRef const &oo) {
		std::lock_guard<std::mutex> lock(mutex);
		tileIndex[index].push_back(oo);
	} */

	static void MergeTileCoordsAtZoom(uint zoom, uint baseZoom, const TileCoordinatesSet &srcTiles, TileCoordinatesSet &dstCoords);
	//static void MergeSingleTileDataAtZoom(TileCoordinates dstIndex, uint zoom, uint baseZoom, const TileIndex &srcTiles, std::vector<OutputObjectRef> &dstTile);
};

TileCoordinatesSet GetTileCoordinates(Box const &clippingBox, unsigned int baseZoom, unsigned int zoom);

std::vector<OutputObjectRef> GetTileData(std::vector<class TileDataSource *> const &sources, TileCoordinates coordinates, unsigned int zoom);

OutputObjectsConstItPair GetObjectsAtSubLayer(std::vector<OutputObjectRef> const &data, uint_least8_t layerNum);

#endif //_TILE_DATA_H
