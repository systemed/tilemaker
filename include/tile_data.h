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
	std::deque<OutputObject> objects;
	
	// rtree index of large objects
	using oo_rtree_param_type = boost::geometry::index::quadratic<128>;
	boost::geometry::index::rtree< std::pair<Box,OutputObjectRef>, oo_rtree_param_type> box_rtree;

	unsigned int baseZoom;

	// Store for generated geometries
	using point_store_t = std::deque<std::pair<NodeID, Point>>;

	using linestring_t = boost::geometry::model::linestring<Point, std::vector, mmap_allocator>;
	using linestring_store_t = std::deque<std::pair<NodeID, linestring_t>>;

	using multi_linestring_t = boost::geometry::model::multi_linestring<linestring_t, std::vector, mmap_allocator>;
	using multi_linestring_store_t = std::deque<std::pair<NodeID, multi_linestring_t>>;

	using polygon_t = boost::geometry::model::polygon<Point, true, true, std::vector, std::vector, mmap_allocator, mmap_allocator>;
	using multi_polygon_t = boost::geometry::model::multi_polygon<polygon_t, std::vector, mmap_allocator>;
	using multi_polygon_store_t = std::deque<std::pair<NodeID, multi_polygon_t>>;

	std::mutex points_store_mutex;
	std::unique_ptr<point_store_t> points_store;
	
	std::mutex linestring_store_mutex;
	std::unique_ptr<linestring_store_t> linestring_store;
	
	std::mutex multi_polygon_store_mutex;
	std::unique_ptr<multi_polygon_store_t> multi_polygon_store;

	std::mutex multi_linestring_store_mutex;
	std::unique_ptr<multi_linestring_store_t> multi_linestring_store;

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

	void AddGeometryToIndex(Linestring const &geom, OutputRefsWithAttributes const &outputs);
	void AddGeometryToIndex(MultiLinestring const &geom, OutputRefsWithAttributes const &outputs);
	void AddGeometryToIndex(MultiPolygon const &geom, OutputRefsWithAttributes const &outputs);

	void AddObjectToTileIndex(TileCoordinates const &index, OutputObjectRef const &oo) {
		std::lock_guard<std::mutex> lock(mutex);
		tileIndex[index].push_back(oo);
	}

	void AddObjectToLargeIndex(Box const &envelope, OutputObjectRef const &oo) {
		std::lock_guard<std::mutex> lock(mutex);
		box_rtree.insert(std::make_pair(envelope, oo));
	}

	void MergeLargeObjects(TileCoordinates dstIndex, uint zoom, std::vector<OutputObjectRef> &dstTile);

	void SortGeometries(unsigned int threadNum);

	std::vector<OutputObjectRef> getTileData(std::vector<bool> const &sortOrders, 
	                                         TileCoordinates coordinates, unsigned int zoom);

	Geometry buildWayGeometry(OutputGeometryType const geomType, NodeID const objectID, const TileBbox &bbox) const;
	LatpLon buildNodeGeometry(OutputGeometryType const geomType, NodeID const objectID, const TileBbox &bbox) const;

	void open() {
		points_store = std::make_unique<point_store_t>();
		linestring_store = std::make_unique<linestring_store_t>();
		multi_polygon_store = std::make_unique<multi_polygon_store_t>();
		multi_linestring_store = std::make_unique<multi_linestring_store_t>();
	}
	void reportSize() const;
	
	// Accessors for generated geometries
	using handle_t = void *;

	template<typename T>
	void store_point(NodeID id, T const &input) {	
		std::lock_guard<std::mutex> lock(points_store_mutex);
		points_store->emplace_back(id, input);		   	
	}

	Point const &retrieve_point(NodeID id) const {
		auto iter = std::lower_bound(points_store->begin(), points_store->end(), id, [](auto const &e, auto id) { 
			return e.first < id; 
		});
		if(iter == points_store->end() || iter->first != id)
			throw std::out_of_range("Could not find generated node with id " + std::to_string(id));
		return iter->second;
	}
	
	template<typename Input>
	void store_linestring(NodeID id, Input const &src) {
		linestring_t dst(src.begin(), src.end());
		std::lock_guard<std::mutex> lock(linestring_store_mutex);
		linestring_store->emplace_back(id, std::move(dst));
	}

	linestring_t const &retrieve_linestring(NodeID id) const {
		auto iter = std::lower_bound(linestring_store->begin(), linestring_store->end(), id, [](auto const &e, auto id) { 
			return e.first < id; 
		});
		if(iter == linestring_store->end() || iter->first != id)
			throw std::out_of_range("Could not find generated linestring with id " + std::to_string(id));
		return iter->second;
	}
	
	template<typename Input>
	void store_multi_linestring(NodeID id, Input const &src) {
		multi_linestring_t dst;
		dst.resize(src.size());
		for (std::size_t i=0; i<src.size(); ++i) {
			boost::geometry::assign(dst[i], src[i]);
		}
		std::lock_guard<std::mutex> lock(multi_linestring_store_mutex);
		multi_linestring_store->emplace_back(id, std::move(dst));
	}

	multi_linestring_t const &retrieve_multi_linestring(NodeID id) const {
		auto iter = std::lower_bound(multi_linestring_store->begin(), multi_linestring_store->end(), id, [](auto const &e, auto id) { 
			return e.first < id; 
		});
		if(iter == multi_linestring_store->end() || iter->first != id)
			throw std::out_of_range("Could not find generated multi-linestring with id " + std::to_string(id));
		return iter->second;
	}

	template<typename Input>
	void store_multi_polygon(NodeID id, Input const &src) {
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
		multi_polygon_store->emplace_back(id, std::move(dst));
	}

	multi_polygon_t const &retrieve_multi_polygon(NodeID id) const {
		auto iter = std::lower_bound(multi_polygon_store->begin(), multi_polygon_store->end(), id, [](auto const &e, auto id) { 
			return e.first < id; 
		});
		if(iter == multi_polygon_store->end() || iter->first != id)
			throw std::out_of_range("Could not find generated multi polygon with id " + std::to_string(id));
		return iter->second;
	}


private:	
	static void MergeTileCoordsAtZoom(uint zoom, uint baseZoom, const TileIndex &srcTiles, TileCoordinatesSet &dstCoords);
	static void MergeSingleTileDataAtZoom(TileCoordinates dstIndex, uint zoom, uint baseZoom, const TileIndex &srcTiles, std::vector<OutputObjectRef> &dstTile);
};

TileCoordinatesSet GetTileCoordinates(std::vector<class TileDataSource *> const &sources, unsigned int zoom);

OutputObjectsConstItPair GetObjectsAtSubLayer(std::vector<OutputObjectRef> const &data, uint_least8_t layerNum);

#endif //_TILE_DATA_H
