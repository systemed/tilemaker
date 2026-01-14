/*! \file */
#ifndef _TILE_DATA_H
#define _TILE_DATA_H

#include <map>
#include <set>
#include <vector>
#include <memory>
#include "tile_data_base.h"
#include "append_vector.h"
#include "clip_cache.h"
#include "mmap_allocator.h"
#include "tile_coordinates_set.h"

typedef std::vector<class TileDataSource *> SourceList;

class TileBbox;

// Declaration only - implementation in tile_sorting.cpp to avoid
// boost::sort conflicts with boost::geometry in Boost 1.89+
template<typename OO> void finalizeObjects(
	const std::string& name,
	const size_t& threadNum,
	const unsigned int& indexZoom,
	typename std::vector<AppendVectorNS::AppendVector<OO>>::iterator begin,
	typename std::vector<AppendVectorNS::AppendVector<OO>>::iterator end,
	typename std::vector<std::vector<OO>>& lowZoom
);

// Extern template declarations to prevent implicit instantiation
extern template void finalizeObjects<OutputObjectXY>(
	const std::string& name,
	const size_t& threadNum,
	const unsigned int& indexZoom,
	std::vector<AppendVectorNS::AppendVector<OutputObjectXY>>::iterator begin,
	std::vector<AppendVectorNS::AppendVector<OutputObjectXY>>::iterator end,
	std::vector<std::vector<OutputObjectXY>>& lowZoom
);

extern template void finalizeObjects<OutputObjectXYID>(
	const std::string& name,
	const size_t& threadNum,
	const unsigned int& indexZoom,
	std::vector<AppendVectorNS::AppendVector<OutputObjectXYID>>::iterator begin,
	std::vector<AppendVectorNS::AppendVector<OutputObjectXYID>>::iterator end,
	std::vector<std::vector<OutputObjectXYID>>& lowZoom
);

template<typename OO> void collectTilesWithObjectsAtZoomTemplate(
	const unsigned int& indexZoom,
	const typename std::vector<AppendVectorNS::AppendVector<OO>>::iterator objects,
	const size_t size,
	std::vector<std::shared_ptr<TileCoordinatesSet>>& zooms
) {
	size_t maxZoom = zooms.size() - 1;
	uint16_t z6OffsetDivisor = indexZoom >= CLUSTER_ZOOM ? (1 << (indexZoom - CLUSTER_ZOOM)) : 1;
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
			TileCoordinate x = baseX / (1 << (indexZoom - maxZoom));
			TileCoordinate y = baseY / (1 << (indexZoom - maxZoom));

			if (lastX != x || lastY != y) {
				lastX = x;
				lastY = y;

				for (int zoom = maxZoom; zoom >= 0; zoom--) {
					zooms[zoom]->set(x, y);
					x /= 2;
					y /= 2;
				}
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

template<typename OO> void collectLowZoomObjectsForTile(
	const unsigned int& indexZoom,
	typename std::vector<std::vector<OO>> objects,
	unsigned int zoom,
	const TileCoordinates& dstIndex,
	std::vector<OutputObjectID>& output
) {
	if (zoom >= CLUSTER_ZOOM)
		throw std::runtime_error("collectLowZoomObjectsForTile should not be called for high zooms");

	uint16_t z6OffsetDivisor = indexZoom >= CLUSTER_ZOOM ? (1 << (indexZoom - CLUSTER_ZOOM)) : 1;

	for (size_t i = 0; i < objects.size(); i++) {
		const size_t z6x = i / CLUSTER_ZOOM_WIDTH;
		const size_t z6y = i % CLUSTER_ZOOM_WIDTH;

		for (size_t j = 0; j < objects[i].size(); j++) {
			// Compute the x, y at the base zoom level
			TileCoordinate baseX = z6x * z6OffsetDivisor + objects[i][j].x;
			TileCoordinate baseY = z6y * z6OffsetDivisor + objects[i][j].y;

			// Translate the x, y at the requested zoom level
			TileCoordinate x = baseX / (1 << (indexZoom - zoom));
			TileCoordinate y = baseY / (1 << (indexZoom - zoom));

			if (dstIndex.x == x && dstIndex.y == y) {
				if (objects[i][j].oo.minZoom <= zoom) {
					output.push_back(outputObjectWithId(objects[i][j]));
				}
			}
		}
	}
}

template<typename OO> void collectObjectsForTileTemplate(
	const unsigned int& indexZoom,
	typename std::vector<AppendVectorNS::AppendVector<OO>>::iterator objects,
	size_t iStart,
	size_t iEnd,
	unsigned int zoom,
	TileCoordinates dstIndex,
	std::vector<OutputObjectID>& output
) {
	if (zoom < CLUSTER_ZOOM)
		throw std::runtime_error("collectObjectsForTileTemplate should not be called for low zooms");

	// When base zoom is z15 or higher, we need to scale down to z14.
	unsigned int clampedZoom = zoom;
	while(clampedZoom > indexZoom) {
		clampedZoom--;
		dstIndex.x /= 2;
		dstIndex.y /= 2;
	}

	uint16_t z6OffsetDivisor = indexZoom >= CLUSTER_ZOOM ? (1 << (indexZoom - CLUSTER_ZOOM)) : 1;

	for (size_t i = iStart; i < iEnd; i++) {
		// If z >= 6, we can compute the exact bounds within the objects array.
		// Translate to the base zoom, then do a binary search to find
		// the starting point.
		TileCoordinate z6x = dstIndex.x / (1 << (clampedZoom - CLUSTER_ZOOM));
		TileCoordinate z6y = dstIndex.y / (1 << (clampedZoom - CLUSTER_ZOOM));

		TileCoordinate baseX = dstIndex.x * (1 << (indexZoom - clampedZoom));
		TileCoordinate baseY = dstIndex.y * (1 << (indexZoom - clampedZoom));

		Z6Offset needleX = baseX - z6x * z6OffsetDivisor;
		Z6Offset needleY = baseY - z6y * z6OffsetDivisor;

		// Kind of gross that we have to do this. Might be better if we split
		// into two arrays, one of x/y and one of OOs. Would have better locality for
		// searching, too.
		OutputObject dummyOo(POINT_, 0, 0, 0, 0);
		const size_t bz = indexZoom;

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
			TileCoordinate x = baseX / (1 << (indexZoom - clampedZoom));
			TileCoordinate y = baseY / (1 << (indexZoom - clampedZoom));

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
	}
}

class TileDataSource {
public:
	// Store for generated geometries
	using point_store_t = std::vector<Point>;

	using linestring_t = boost::geometry::model::linestring<Point, std::vector, mmap_allocator>;
	using linestring_store_t = std::vector<linestring_t>;

	using multi_linestring_t = boost::geometry::model::multi_linestring<linestring_t, std::vector, mmap_allocator>;
	using multi_linestring_store_t = std::vector<multi_linestring_t>;

	using polygon_t = boost::geometry::model::polygon<Point, true, true, std::vector, std::vector, mmap_allocator, mmap_allocator>;
	using multi_polygon_t = boost::geometry::model::multi_polygon<polygon_t, std::vector, mmap_allocator>;
	using multi_polygon_store_t = std::vector<multi_polygon_t>;

	std::mutex storeMutex;
	// Threads can grab one of the stores and work on them in a thread local.
	std::vector<std::pair<size_t, point_store_t*>> availablePointStoreLeases;
	std::vector<std::pair<size_t, linestring_store_t*>> availableLinestringStoreLeases;
	std::vector<std::pair<size_t, multi_linestring_store_t*>> availableMultiLinestringStoreLeases;
	std::vector<std::pair<size_t, multi_polygon_store_t*>> availableMultiPolygonStoreLeases;

	virtual std::string name() const = 0;

protected:	
	size_t numShards;
	uint8_t shardBits;
	std::mutex mutex;
	bool includeID;
	uint16_t z6OffsetDivisor;

	// Guards objects, objectsWithIds.
	std::vector<std::mutex> objectsMutex;
	// The top-level vector has 1 entry per z6 tile, indexed by x*64 + y
	// The inner vector contains the output objects that are contained in that z6 tile
	//
	// In general, only one of these vectors will be populated.
	//
	// If config.include_ids is true, objectsWithIds will be populated.
	// Otherwise, objects.
	std::vector<AppendVectorNS::AppendVector<OutputObjectXY>> objects;
	std::vector<std::vector<OutputObjectXY>> lowZoomObjects;
	std::vector<AppendVectorNS::AppendVector<OutputObjectXYID>> objectsWithIds;
	std::vector<std::vector<OutputObjectXYID>> lowZoomObjectsWithIds;
	
	// rtree index of large objects
	using oo_rtree_param_type = boost::geometry::index::quadratic<128>;
	boost::geometry::index::rtree< std::pair<Box,OutputObject>, oo_rtree_param_type> boxRtree;
	boost::geometry::index::rtree< std::pair<Box,OutputObjectID>, oo_rtree_param_type> boxRtreeWithIds;

	unsigned int indexZoom;

	std::vector<point_store_t> pointStores;
	std::vector<linestring_store_t> linestringStores;
	std::vector<multi_linestring_store_t> multilinestringStores;
	std::vector<multi_polygon_store_t> multipolygonStores;
	
	ClipCache<MultiPolygon> multiPolygonClipCache;
	ClipCache<MultiLinestring> multiLinestringClipCache;

	std::deque<std::vector<std::tuple<TileCoordinates, OutputObject, uint64_t>>> pendingSmallIndexObjects;

public:
	TileDataSource(size_t threadNum, unsigned int indexZoom, bool includeID);

	void collectTilesWithObjectsAtZoom(std::vector<std::shared_ptr<TileCoordinatesSet>>& zooms);

	void collectTilesWithLargeObjectsAtZoom(std::vector<std::shared_ptr<TileCoordinatesSet>>& zooms);

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
		std::vector<OutputObject>& outputs, // so we can mutate objectID to skip clip cache
		const uint64_t id
	);

	void addObjectToSmallIndex(const TileCoordinates& index, const OutputObject& oo, uint64_t id);
	void addObjectToSmallIndex(const TileCoordinates& index, const OutputObject& oo, uint64_t id, bool needsLock);
	void addObjectToSmallIndexUnsafe(const TileCoordinates& index, const OutputObject& oo, uint64_t id);

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

	virtual Geometry buildWayGeometry(OutputGeometryType const geomType, NodeID const objectID, const TileBbox &bbox);
	virtual LatpLon buildNodeGeometry(NodeID const objectID, const TileBbox &bbox) const;

	void open() {
		// Put something at index 0 of all stores so that 0 can be used
		// as a sentinel.
		pointStores[0].push_back(Point(0,0));
		linestringStores[0].push_back(linestring_t());
		multipolygonStores[0].push_back(multi_polygon_t());
		multilinestringStores[0].push_back(multi_linestring_t());
	}
	void reportSize() const;
	
	// Accessors for generated geometries
	using handle_t = void *;

	NodeID storePoint(Point const &input);

	inline size_t getShard(NodeID id) const {
		// Note: we only allocate 34 bits for the IDs. This allows us to
		// use bits 35 and 36 for TileDataSource-specific handling (e.g.,
		// OsmMemTiles may want to generate points/ways on the fly by
		// referring to the WayStore).

		return id >> (TILE_DATA_ID_SIZE - shardBits);
	}

	virtual void populateMultiPolygon(MultiPolygon& dst, NodeID objectID);

	unsigned int getIndexZoom() const { return indexZoom; }
	inline size_t getId(NodeID id) const {
		return id & (~(~0ull << (TILE_DATA_ID_SIZE - shardBits)));
	}

	const Point& retrievePoint(NodeID id) const {
		const auto& shardId = getShard(id);
		const auto& shard = pointStores[shardId];
		const auto offset = getId(id);
		if (offset > shard.size()) throw std::out_of_range("Could not find generated node with id " + std::to_string(id) + ", shard " + std::to_string(shardId) + ", offset=" + std::to_string(offset));
		return shard.at(offset);
	}
	
	NodeID storeLinestring(const Linestring& src);

	const linestring_t& retrieveLinestring(NodeID id) const {
		const auto& shardId = getShard(id);
		const auto& shard = linestringStores[shardId];
		const auto offset = getId(id);
		if (offset > shard.size()) throw std::out_of_range("Could not find generated linestring with id " + std::to_string(id) + ", shard " + std::to_string(shardId) + ", offset=" + std::to_string(offset));
		return shard.at(offset);
	}
	
	NodeID storeMultiLinestring(const MultiLinestring& src);

	multi_linestring_t const &retrieveMultiLinestring(NodeID id) const {
		const auto& shardId = getShard(id);
		const auto& shard = multilinestringStores[shardId];
		const auto offset = getId(id);
		if (offset > shard.size()) throw std::out_of_range("Could not find generated multi-linestring with id " + std::to_string(id) + ", shard " + std::to_string(shardId) + ", offset=" + std::to_string(offset));
		return shard.at(offset);
	}

	NodeID storeMultiPolygon(const MultiPolygon& src);

	multi_polygon_t const &retrieveMultiPolygon(NodeID id) const {
		const auto& shardId = getShard(id);
		const auto& shard = multipolygonStores[shardId];
		const auto offset = getId(id);
		if (offset > shard.size()) throw std::out_of_range("Could not find generated multi-polygon with id " + std::to_string(id) + ", shard " + std::to_string(shardId) + ", offset=" + std::to_string(offset));
		return shard.at(offset);
	}
};

void populateTilesAtZoom(
	const std::vector<class TileDataSource *>& sources,
	std::vector<std::shared_ptr<TileCoordinatesSet>>& zooms
);

#endif //_TILE_DATA_H
