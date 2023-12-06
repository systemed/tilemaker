/*! \file */ 
#ifndef _TILE_DATA_H
#define _TILE_DATA_H

#include <map>
#include <set>
#include <vector>
#include <memory>
#include <boost/sort/sort.hpp>
#include "output_object.h"

typedef std::set<TileCoordinates, TileCoordinatesCompare> TileCoordinatesSet;

typedef std::vector<class TileDataSource *> SourceList;

// We cluster output objects by z6 tile
#define CLUSTER_ZOOM 6
#define CLUSTER_ZOOM_WIDTH (1 << CLUSTER_ZOOM)
#define CLUSTER_ZOOM_AREA (CLUSTER_ZOOM_WIDTH * CLUSTER_ZOOM_WIDTH)

struct OutputObjectXY {
	OutputObject oo;
	Z6Offset x;
	Z6Offset y;
};

struct OutputObjectXYID {
	OutputObject oo;
	Z6Offset x;
	Z6Offset y;
	uint64_t id;
};

template<typename OO> void finalizeObjects(
	const size_t& threadNum,
	const unsigned int& baseZoom,
	typename std::vector<std::vector<OO>>::iterator begin,
	typename std::vector<std::vector<OO>>::iterator end
	) {
	for (typename std::vector<std::vector<OO>>::iterator it = begin; it != end; it++) {
		if (it->size() == 0)
			continue;

		it->shrink_to_fit();

		// If the user is doing a a small extract, there are few populated
		// entries in `object`.
		//
		// e.g. Colorado has ~9 z6 tiles, 1 of which has 95% of its output
		// objects.
		//
		// This optimizes for the small extract case by doing:
		// - for each vector in objects
		//   - do a multi-threaded sort of vector
		//
		// For small extracts, this ensures that all threads are used even if
		// only a handful of entries in `objects` are non-empty.
		//
		// For a global extract, this will have some overhead of repeatedly
		// setting up/tearing down threads. In that case, it would be 
		// better to assign chunks of `objects` to each thread.
		//
		// That's a future performance improvement, so deferring for now.
		boost::sort::block_indirect_sort(
			it->begin(),
			it->end(), 
			[baseZoom](const OO& a, const OO& b) {
				// Cluster by parent zoom, so that a subsequent search
				// can find a contiguous range of entries for any tile
				// at zoom 6 or higher.
				const size_t aX = a.x;
				const size_t aY = a.y;
				const size_t bX = b.x;
				const size_t bY = b.y;
				for (size_t z = CLUSTER_ZOOM; z <= baseZoom; z++) {
					const auto aXz = aX / (1 << (baseZoom - z));
					const auto bXz = bX / (1 << (baseZoom - z));
					if (aXz != bXz)
						return aXz < bXz;

					const auto aYz = aY / (1 << (baseZoom - z));
					const auto bYz = bY / (1 << (baseZoom - z));

					if (aYz != bYz)
						return aYz < bYz;
				}

				return false;
			},
			threadNum
		);

	}
}

template<typename OO> void collectTilesWithObjectsAtZoomTemplate(
	const unsigned int& baseZoom,
	const typename std::vector<std::vector<OO>>::iterator objects,
	const size_t size,
	const unsigned int zoom,
	TileCoordinatesSet& output
) {
	uint16_t z6OffsetDivisor = baseZoom >= CLUSTER_ZOOM ? (1 << (baseZoom - CLUSTER_ZOOM)) : 1;
	int64_t lastX = -1;
	int64_t lastY = -1;
	for (size_t i = 0; i < size; i++) {
		const size_t z6x = i / CLUSTER_ZOOM_WIDTH;
		const size_t z6y = i % CLUSTER_ZOOM_WIDTH;

		for (size_t j = 0; j < objects[i].size(); j++) {
			// Compute the x, y at the base zoom level
			TileCoordinate baseX = z6x * z6OffsetDivisor + objects[i][j].x;
			TileCoordinate baseY = z6y * z6OffsetDivisor + objects[i][j].y;

			// Translate the x, y at the requested zoom level
			TileCoordinate x = baseX / (1 << (baseZoom - zoom));
			TileCoordinate y = baseY / (1 << (baseZoom - zoom));

			if (lastX != x || lastY != y) {
				output.insert(TileCoordinates(x, y));
				lastX = x;
				lastY = y;
			}
		}
	}
}

template<typename OO>
OutputObjectID outputObjectWithId(const OO& input) {
	return OutputObjectID({ input.oo, 0 });
}

template<>
inline OutputObjectID outputObjectWithId<OutputObjectXYID>(const OutputObjectXYID& input) {
	return OutputObjectID({ input.oo, input.id });
}

template<typename OO> void collectObjectsForTileTemplate(
	const unsigned int& baseZoom,
	typename std::vector<std::vector<OO>>::iterator objects,
	size_t iStart,
	size_t iEnd,
	unsigned int zoom,
	const TileCoordinates& dstIndex,
	std::vector<OutputObjectID>& output
) {
	uint16_t z6OffsetDivisor = baseZoom >= CLUSTER_ZOOM ? (1 << (baseZoom - CLUSTER_ZOOM)) : 1;

	for (size_t i = iStart; i < iEnd; i++) {
		const size_t z6x = i / CLUSTER_ZOOM_WIDTH;
		const size_t z6y = i % CLUSTER_ZOOM_WIDTH;

		if (zoom >= CLUSTER_ZOOM) {
			// If z >= 6, we can compute the exact bounds within the objects array.
			// Translate to the base zoom, then do a binary search to find
			// the starting point.
			TileCoordinate z6x = dstIndex.x / (1 << (zoom - CLUSTER_ZOOM));
			TileCoordinate z6y = dstIndex.y / (1 << (zoom - CLUSTER_ZOOM));

			TileCoordinate baseX = dstIndex.x * (1 << (baseZoom - zoom));
			TileCoordinate baseY = dstIndex.y * (1 << (baseZoom - zoom));

			Z6Offset needleX = baseX - z6x * z6OffsetDivisor;
			Z6Offset needleY = baseY - z6y * z6OffsetDivisor;

			// Kind of gross that we have to do this. Might be better if we split
			// into two arrays, one of x/y and one of OOs. Would have better locality for
			// searching, too.
			OutputObject dummyOo(POINT_, 0, 0, 0, 0);
			const size_t bz = baseZoom;

			const OO targetXY = {dummyOo, needleX, needleY };
			auto iter = std::lower_bound(
				objects[i].begin(),
				objects[i].end(),
				targetXY,
				[bz](const OO& a, const OO& b) {
					// Cluster by parent zoom, so that a subsequent search
					// can find a contiguous range of entries for any tile
					// at zoom 6 or higher.
					const size_t aX = a.x;
					const size_t aY = a.y;
					const size_t bX = b.x;
					const size_t bY = b.y;
					for (size_t z = CLUSTER_ZOOM; z <= bz; z++) {
						const auto aXz = aX / (1 << (bz - z));
						const auto aYz = aY / (1 << (bz - z));
						const auto bXz = bX / (1 << (bz - z));
						const auto bYz = bY / (1 << (bz - z));

						if (aXz != bXz)
							return aXz < bXz;

						if (aYz != bYz)
							return aYz < bYz;
					}
					return false;
				}
			);
			for (; iter != objects[i].end(); iter++) {
				// Compute the x, y at the base zoom level
				TileCoordinate baseX = z6x * z6OffsetDivisor + iter->x;
				TileCoordinate baseY = z6y * z6OffsetDivisor + iter->y;

				// Translate the x, y at the requested zoom level
				TileCoordinate x = baseX / (1 << (baseZoom - zoom));
				TileCoordinate y = baseY / (1 << (baseZoom - zoom));

				if (dstIndex.x == x && dstIndex.y == y) {
					if (iter->oo.minZoom <= zoom) {
						output.push_back(outputObjectWithId(*iter));
					}
				} else {
					// Short-circuit when we're confident we'd no longer see relevant matches.
					// We've ordered the entries in `objects` such that all objects that
					// share the same tile at any zoom are in contiguous runs.
					//
					// Thus, as soon as we fail to find a match, we can stop looking.
					break;
				}

			}
		} else {
			for (size_t j = 0; j < objects[i].size(); j++) {
				// Compute the x, y at the base zoom level
				TileCoordinate baseX = z6x * z6OffsetDivisor + objects[i][j].x;
				TileCoordinate baseY = z6y * z6OffsetDivisor + objects[i][j].y;

				// Translate the x, y at the requested zoom level
				TileCoordinate x = baseX / (1 << (baseZoom - zoom));
				TileCoordinate y = baseY / (1 << (baseZoom - zoom));

				if (dstIndex.x == x && dstIndex.y == y) {
					if (objects[i][j].oo.minZoom <= zoom) {
						output.push_back(outputObjectWithId(objects[i][j]));
					}
				}
			}
		}
	}
}

class TileDataSource {

protected:	
	std::mutex mutex;
	bool includeID;
	uint16_t z6OffsetDivisor;

	// Guards objects, objectsWithIds.
	mutable std::vector<std::mutex> objectsMutex;
	// The top-level vector has 1 entry per z6 tile, indexed by x*64 + y
	// The inner vector contains the output objects that are contained in that z6 tile
	//
	// In general, only one of these vectors will be populated.
	//
	// If config.include_ids is true, objectsWithIds will be populated.
	// Otherwise, objects.
	std::vector<std::vector<OutputObjectXY>> objects;
	std::vector<std::vector<OutputObjectXYID>> objectsWithIds;
	
	// rtree index of large objects
	using oo_rtree_param_type = boost::geometry::index::quadratic<128>;
	boost::geometry::index::rtree< std::pair<Box,OutputObject>, oo_rtree_param_type> boxRtree;
	boost::geometry::index::rtree< std::pair<Box,OutputObjectID>, oo_rtree_param_type> boxRtreeWithIds;

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

	// Some polygons are very large, e.g. Hudson Bay covers hundreds of thousands of
	// z14 tiles.
	//
	// Because it is very large, it is expensive to clip it once, let alone hundreds
	// of thousands of times.
	//
	// Fortunately, we can avoid redundant clipping. If we clip a polygon and observe
	// that it fills its entire tile, we know that it must fill all its descendant
	// tiles at higher zooms.
	//
	// We divide the map into 4,096 z6 tiles, then track polygons within that z6
	// tile if they'd cover at least an entire z9 tile.
	//
	// Because z9 is 3 zooms higher than z6, we can use a uint64_t as a bitset
	// to track the tiles that would be entirely filled.
	//
	// To minimize lock contention, we shard by NodeID.
	std::vector<std::map<std::pair<uint32_t, NodeID>, uint64_t>> jumboCoveringPolygons;
	// Like above, but for the case where we're in the hole of the polygon,
	// and so render nothing.
	std::vector<std::map<std::pair<uint32_t, NodeID>, uint64_t>> jumboExcludedPolygons;

	// Like above, but cover z10..z13, rather than z6..z9
	std::vector<std::map<std::pair<uint32_t, NodeID>, uint64_t>> largeCoveringPolygons;
	std::vector<std::map<std::pair<uint32_t, NodeID>, uint64_t>> largeExcludedPolygons;


public:
	TileDataSource(size_t threadNum, unsigned int baseZoom, bool includeID);

	void collectTilesWithObjectsAtZoom(uint zoom, TileCoordinatesSet& output);

	void collectTilesWithLargeObjectsAtZoom(uint zoom, TileCoordinatesSet& output);

	void collectObjectsForTile(uint zoom, TileCoordinates dstIndex, std::vector<OutputObjectID>& output);
	void finalize(size_t threadNum);

	void addGeometryToIndex(
		const Linestring& geom,
		const std::vector<OutputObject>& outputs,
		const uint64_t id
	);
	void addGeometryToIndex(
		const MultiLinestring& geom,
		const std::vector<OutputObject>& outputs,
		const uint64_t id
	);
	void addGeometryToIndex(
		const MultiPolygon& geom,
		const std::vector<OutputObject>& outputs,
		const uint64_t id
	);

	void addObjectToSmallIndex(const TileCoordinates& index, const OutputObject& oo, uint64_t id);

	void addObjectToLargeIndex(const Box& envelope, const OutputObject& oo, uint64_t id) {
		std::lock_guard<std::mutex> lock(mutex);
		if (id == 0 || !includeID)
			boxRtree.insert(std::make_pair(envelope, oo));
		else
			boxRtreeWithIds.insert(std::make_pair(envelope, OutputObjectID({oo, id})));
	}

	void collectLargeObjectsForTile(uint zoom, TileCoordinates dstIndex, std::vector<OutputObjectID>& output);

	std::vector<OutputObjectID> getObjectsForTile(
		const std::vector<bool>& sortOrders, 
		unsigned int zoom,
		TileCoordinates coordinates
	);

	Geometry buildWayGeometry(OutputGeometryType const geomType, NodeID const objectID, const TileBbox &bbox);
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

	NodeID store_point(Point const &input) {
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
	void updateLargePolygonMaps(NodeID objectID, const TileBbox& bbox, const MultiPolygon& mp);
};

TileCoordinatesSet getTilesAtZoom(
	const std::vector<class TileDataSource *>& sources,
	unsigned int zoom
);

#endif //_TILE_DATA_H
