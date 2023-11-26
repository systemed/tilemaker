/*! \file */ 
#ifndef _TILE_DATA_H
#define _TILE_DATA_H

#include <map>
#include <set>
#include <vector>
#include <memory>
#include "output_object.h"

typedef std::vector<OutputObject>::const_iterator OutputObjectsConstIt;
typedef std::pair<OutputObjectsConstIt, OutputObjectsConstIt> OutputObjectsConstItPair;
// TODO: remove TileIndex ?
typedef std::map<TileCoordinates, std::vector<OutputObject>, TileCoordinatesCompare> TileIndex;
typedef std::set<TileCoordinates, TileCoordinatesCompare> TileCoordinatesSet;

typedef std::vector<class TileDataSource *> SourceList;

class TileDataSource {

protected:	
	std::mutex mutex;

	uint16_t z6OffsetDivisor;
	// The top-level vector has 1 entry per z6 tile, indexed by x*64 + y
	// The inner vector contains the output objects that are contained in that z6 tile
	std::vector<std::vector<OutputObject>> objects;

	// The top-level vector has 1 entry per z6 tile, indexed by x*64 + y
	// The inner vector contains the output object's base zoom tile coordinates,
	// relative to the z6 parent tile.
	// e.g.    given a z14 tile of: 4528, 5991
	//         then its z6 tile is: 17, 23
	//           and its offset is: 178, 103 (given by: 4528 - (Z6x * Z6_OFFSET_DIVISOR))
	//   ...where Z6_OFFSET_DIVISOR is 2^max(basezoom - 6, 0)
	std::vector<std::vector<std::pair<Z6Offset, Z6Offset>>> z6Offsets;
	
	// rtree index of large objects
	using oo_rtree_param_type = boost::geometry::index::quadratic<128>;
	boost::geometry::index::rtree< std::pair<Box,OutputObject>, oo_rtree_param_type> box_rtree;

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
	TileDataSource(unsigned int baseZoom);

	void collectTilesWithObjectsAtZoom(uint zoom, TileCoordinatesSet& output);

	void collectTilesWithLargeObjectsAtZoom(uint zoom, TileCoordinatesSet& output);

	void collectObjectsForTile(uint zoom, TileCoordinates dstIndex, std::vector<OutputObject>& output);

	void AddGeometryToIndex(const Linestring& geom, const std::vector<OutputObject>& outputs);
	void AddGeometryToIndex(const MultiLinestring& geom, const std::vector<OutputObject>& outputs);
	void AddGeometryToIndex(const MultiPolygon& geom, const std::vector<OutputObject>& outputs);

	void addObjectToTileIndex(const TileCoordinates& index, const OutputObject& oo);

	void AddObjectToLargeIndex(const Box& envelope, const OutputObject& oo) {
		std::lock_guard<std::mutex> lock(mutex);
		box_rtree.insert(std::make_pair(envelope, oo));
	}

	void collectLargeObjectsForTile(uint zoom, TileCoordinates dstIndex, std::vector<OutputObject>& output);

	std::vector<OutputObject> getObjectsForTile(
		const std::vector<bool>& sortOrders, 
		unsigned int zoom,
		TileCoordinates coordinates
	);

	Geometry buildWayGeometry(OutputGeometryType const geomType, NodeID const objectID, const TileBbox &bbox) const;
	LatpLon buildNodeGeometry(OutputGeometryType const geomType, NodeID const objectID, const TileBbox &bbox) const;

	void open() {
		point_store = std::make_unique<point_store_t>();
		linestring_store = std::make_unique<linestring_store_t>();
		multi_polygon_store = std::make_unique<multi_polygon_store_t>();
		multi_linestring_store = std::make_unique<multi_linestring_store_t>();

		// Put something at index 0 of all stores so that 0 can be used
		// as a sentinel.
		point_store->push_back(Point(0,0));
		linestring_store->push_back(linestring_t());
		multi_polygon_store->push_back(multi_polygon_t());
		multi_linestring_store->push_back(multi_linestring_t());
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
};

TileCoordinatesSet getTilesAtZoom(
	const std::vector<class TileDataSource *>& sources,
	unsigned int zoom
);

OutputObjectsConstItPair getObjectsAtSubLayer(
	const std::vector<OutputObject>& data,
	uint_least8_t layerNum
);


#endif //_TILE_DATA_H
