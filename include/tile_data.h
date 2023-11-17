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

typedef std::vector<class TileDataSource *> SourceList;

class TileDataSource {

protected:	
	std::mutex mutex;
	TileIndex tileIndex;
	std::vector<std::deque<OutputObject>> objects;
	std::vector<std::mutex> objectsMutex;
	
	// rtree index of large objects
	using oo_rtree_param_type = boost::geometry::index::quadratic<128>;
	boost::geometry::index::rtree< std::pair<Box,OutputObjectRef>, oo_rtree_param_type> box_rtree;

	unsigned int baseZoom;

	// Store for generated geometries
	using point_store_t = std::vector<Point>;

	using linestring_t = boost::geometry::model::linestring<Point, std::vector, mmap_allocator>;
	using linestring_store_t = std::vector<linestring_t>;

	using multi_linestring_t = boost::geometry::model::multi_linestring<linestring_t, std::vector, mmap_allocator>;
	using multi_linestring_store_t = std::vector<multi_linestring_t>;

	using polygon_t = boost::geometry::model::polygon<Point, true, true, std::vector, std::vector, mmap_allocator, mmap_allocator>;
	using multi_polygon_t = boost::geometry::model::multi_polygon<polygon_t, std::vector, mmap_allocator>;
	using multi_polygon_store_t = std::vector<multi_polygon_t>;

	std::mutex point_store_mutex;
	std::unique_ptr<point_store_t> point_store;
	
	std::mutex linestring_store_mutex;
	std::unique_ptr<linestring_store_t> linestring_store;
	
	std::mutex multi_polygon_store_mutex;
	std::unique_ptr<multi_polygon_store_t> multi_polygon_store;

	std::mutex multi_linestring_store_mutex;
	std::unique_ptr<multi_linestring_store_t> multi_linestring_store;

public:
	TileDataSource(unsigned int baseZoom) 
		: baseZoom(baseZoom), objects(16), objectsMutex(16)
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
		size_t hash = (uint64_t)&oo ^ 0x9e3779b97f4a7c16;
		size_t shard = hash % objectsMutex.size();
		std::lock_guard<std::mutex> lock(objectsMutex[shard]);
		objects[shard].push_back(oo);
		return &objects[shard].back();
	}

	void AddGeometryToIndex(Linestring const &geom, std::vector<OutputObjectRef> const &outputs);
	void AddGeometryToIndex(MultiLinestring const &geom, std::vector<OutputObjectRef> const &outputs);
	void AddGeometryToIndex(MultiPolygon const &geom, std::vector<OutputObjectRef> const &outputs);

	void AddObjectToTileIndex(TileCoordinates const &index, OutputObjectRef const &oo) {
		std::lock_guard<std::mutex> lock(mutex);
		tileIndex[index].push_back(oo);
	}

	void AddObjectToLargeIndex(Box const &envelope, OutputObjectRef const &oo) {
		std::lock_guard<std::mutex> lock(mutex);
		box_rtree.insert(std::make_pair(envelope, oo));
	}

	void MergeLargeObjects(TileCoordinates dstIndex, uint zoom, std::vector<OutputObjectRef> &dstTile);

	std::vector<OutputObjectRef> getTileData(std::vector<bool> const &sortOrders, 
	                                         TileCoordinates coordinates, unsigned int zoom);

	Geometry buildWayGeometry(OutputGeometryType const geomType, NodeID const objectID, const TileBbox &bbox) const;
	LatpLon buildNodeGeometry(OutputGeometryType const geomType, NodeID const objectID, const TileBbox &bbox) const;

	void open() {
		point_store = std::make_unique<point_store_t>();
		linestring_store = std::make_unique<linestring_store_t>();
		multi_polygon_store = std::make_unique<multi_polygon_store_t>();
		multi_linestring_store = std::make_unique<multi_linestring_store_t>();
	}
	void reportSize() const;
	
	// Accessors for generated geometries
	using handle_t = void *;

	template<typename T>
	NodeID store_point(T const &input) {
		std::lock_guard<std::mutex> lock(point_store_mutex);
		NodeID id = point_store->size();
		point_store->emplace_back(input);
		return id;
	}

	Point const &retrieve_point(NodeID id) const {
		if (id > point_store->size()) throw std::out_of_range("Could not find generated node with id " + std::to_string(id));
		return point_store->at(id);
	}
	
	template<typename Input>
	NodeID store_linestring(Input const &src) {
		linestring_t dst(src.begin(), src.end());
		std::lock_guard<std::mutex> lock(linestring_store_mutex);
		NodeID id = linestring_store->size();
		linestring_store->emplace_back(std::move(dst));
		return id;
	}

	linestring_t const &retrieve_linestring(NodeID id) const {
		if (id > linestring_store->size()) throw std::out_of_range("Could not find generated linestring with id " + std::to_string(id));
		return linestring_store->at(id);
	}
	
	template<typename Input>
	NodeID store_multi_linestring(Input const &src) {
		multi_linestring_t dst;
		dst.resize(src.size());
		for (std::size_t i=0; i<src.size(); ++i) {
			boost::geometry::assign(dst[i], src[i]);
		}
		std::lock_guard<std::mutex> lock(multi_linestring_store_mutex);
		NodeID id = multi_linestring_store->size();
		multi_linestring_store->emplace_back(std::move(dst));
		return id;
	}

	multi_linestring_t const &retrieve_multi_linestring(NodeID id) const {
		if (id > multi_linestring_store->size()) throw std::out_of_range("Could not find generated multi-linestring with id " + std::to_string(id));
		return multi_linestring_store->at(id);
	}

	template<typename Input>
	NodeID store_multi_polygon(Input const &src) {
		multi_polygon_t dst;
		dst.resize(src.size());
		for(std::size_t i = 0; i < src.size(); ++i) {
			dst[i].outer().resize(src[i].outer().size());
			boost::geometry::assign(dst[i].outer(), src[i].outer());

			dst[i].inners().resize(src[i].inners().size());
			for(std::size_t j = 0; j < src[i].inners().size(); ++j) {
				dst[i].inners()[j].resize(src[i].inners()[j].size());
				boost::geometry::assign(dst[i].inners()[j], src[i].inners()[j]);
			}
		}
		std::lock_guard<std::mutex> lock(multi_polygon_store_mutex);
		NodeID id = multi_polygon_store->size();
		multi_polygon_store->emplace_back(std::move(dst));
		return id;
	}

	multi_polygon_t const &retrieve_multi_polygon(NodeID id) const {
		if (id > multi_polygon_store->size()) throw std::out_of_range("Could not find generated multi-polygon with id " + std::to_string(id));
		return multi_polygon_store->at(id);
	}


private:	
	static void MergeTileCoordsAtZoom(uint zoom, uint baseZoom, const TileIndex &srcTiles, TileCoordinatesSet &dstCoords);
	static void MergeSingleTileDataAtZoom(TileCoordinates dstIndex, uint zoom, uint baseZoom, const TileIndex &srcTiles, std::vector<OutputObjectRef> &dstTile);
};

TileCoordinatesSet GetTileCoordinates(std::vector<class TileDataSource *> const &sources, unsigned int zoom);

OutputObjectsConstItPair GetObjectsAtSubLayer(std::vector<OutputObjectRef> const &data, uint_least8_t layerNum);

#endif //_TILE_DATA_H
