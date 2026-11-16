/*! \file */ 
#ifndef _TILE_DATA_H
#define _TILE_DATA_H

#include <algorithm>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <vector>
#include <unordered_set>
#include <tuple>
#include <memory>
#include <functional>
#include <future>
#include <atomic>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/post.hpp>
#include "append_vector.h"
#include "clip_cache.h"
#include "mmap_allocator.h"
#include "tile_coordinates_set.h"
#include "tile_data_base.h"

typedef std::vector<class TileDataSource *> SourceList;

// The TileDataSources alive, for the store leases a thread keeps until it ends (leased_store.h): the
// main thread gives its leases back at exit, after the sources are gone. A source is also told apart
// by its number, as a new source can be at the address of a destroyed one.
uint64_t registerLeaseSource(const TileDataSource *source);
void unregisterLeaseSource(const TileDataSource *source);
std::mutex &leaseSourcesMutex();
bool leaseSourceAlive(const TileDataSource *source, uint64_t number);	// with leaseSourcesMutex() held

class TileBbox;

template<typename OO> void sortOutputObjects(
    const unsigned int indexZoom,
    const size_t threadNum,
	typename AppendVectorNS::AppendVector<OO>::Iterator begin,
	typename AppendVectorNS::AppendVector<OO>::Iterator end
);

template<typename OO> void finalizeObjects(
	const std::string& name,
	const size_t& threadNum,
	const unsigned int& indexZoom,
	typename std::vector<AppendVectorNS::AppendVector<OO>>::iterator begin,
	typename std::vector<AppendVectorNS::AppendVector<OO>>::iterator end,
	typename std::vector<std::vector<OO>>& lowZoom
	) {
#ifdef CLOCK_MONOTONIC
	timespec startTs, endTs;
	clock_gettime(CLOCK_MONOTONIC, &startTs);
#endif
	const size_t shards = end - begin;
	std::mutex progressMutex;
	size_t done = 0;
	auto reportDone = [&](size_t n) {
		std::lock_guard<std::mutex> lock(progressMutex);
		done += n;
		std::cout << "\r" << name << ": finalizing z6 tile " << done << "/" << CLUSTER_ZOOM_AREA;
#ifdef CLOCK_MONOTONIC
		clock_gettime(CLOCK_MONOTONIC, &endTs);
		uint64_t elapsedNs = 1e9 * (endTs.tv_sec - startTs.tv_sec) + endTs.tv_nsec - startTs.tv_nsec;
		std::cout << " (" << std::to_string((uint32_t)(elapsedNs / 1e6)) << " ms)";
#endif
		std::cout << std::flush;
	};

	auto finalizeShard = [&](size_t i, size_t sortThreads) {
		auto it = begin + i;
		// We track a separate copy of low zoom objects to avoid scanning large
		// lists of objects that may be on slow disk storage.
		for (auto objectIt = it->begin(); objectIt != it->end(); objectIt++)
			if (objectIt->oo.minZoom < CLUSTER_ZOOM)
				lowZoom[i].push_back(*objectIt);

		sortOutputObjects<OO>(indexZoom, sortThreads, it->begin(), it->end());
	};

	// Shards are independent. A shard with more than 1/threadNum of all objects
	// (e.g. one of the few populated z6 tiles of a small extract) is sorted on its
	// own using all threads. The others are sorted in parallel, one thread each,
	// largest first.
	size_t total = 0, empty = 0;
	for (auto it = begin; it != end; it++) total += it->size();
	std::vector<size_t> big, small;
	for (size_t i = 0; i < shards; i++) {
		const size_t n = (begin + i)->size();
		if (n == 0) empty++;
		else if (threadNum <= 1 || n * threadNum > total) big.push_back(i);
		else small.push_back(i);
	}
	std::sort(small.begin(), small.end(), [&](size_t a, size_t b) { return (begin + a)->size() > (begin + b)->size(); });
	reportDone(empty);

	for (size_t i : big) {
		finalizeShard(i, threadNum);
		reportDone(1);
	}

	if (!small.empty()) {
		// Each sort copies its shard into a buffer, so cap the objects being
		// sorted at once (a single shard may always run).
		const size_t budget = std::max<size_t>(total / 8, (begin + small.front())->size());
		size_t inFlight = 0;
		std::mutex budgetMutex;
		std::condition_variable budgetFreed;
		boost::asio::thread_pool pool(threadNum);
		for (size_t i : small) {
			boost::asio::post(pool, [&, i]() {
				const size_t n = (begin + i)->size();
				{
					std::unique_lock<std::mutex> lock(budgetMutex);
					budgetFreed.wait(lock, [&]() { return inFlight == 0 || inFlight + n <= budget; });
					inFlight += n;
				}
				finalizeShard(i, 1);
				{
					std::lock_guard<std::mutex> lock(budgetMutex);
					inFlight -= n;
				}
				budgetFreed.notify_all();
				reportDone(1);
			});
		}
		pool.join();
	}

	std::cout << std::endl;
}

// Set the tiles holding the objects of z6 tile i (shard), and their parent tiles.
template<typename OO> void collectTilesWithObjectsInShardTemplate(
	const unsigned int& indexZoom,
	AppendVectorNS::AppendVector<OO>& shard,
	const size_t i,
	std::vector<std::shared_ptr<TileCoordinatesSet>>& zooms
) {
	size_t maxZoom = zooms.size() - 1;
	uint16_t z6OffsetDivisor = indexZoom >= CLUSTER_ZOOM ? (1 << (indexZoom - CLUSTER_ZOOM)) : 1;
	int64_t lastX = -1;
	int64_t lastY = -1;
	const size_t z6x = i / CLUSTER_ZOOM_WIDTH;
	const size_t z6y = i % CLUSTER_ZOOM_WIDTH;

	for (size_t j = 0; j < shard.size(); j++) {
		// Compute the x, y at the base zoom level
		TileCoordinate baseX = z6x * z6OffsetDivisor + shard[j].x;
		TileCoordinate baseY = z6y * z6OffsetDivisor + shard[j].y;

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

template<typename OO> void collectTilesWithObjectsAtZoomTemplate(
	const unsigned int& indexZoom,
	const typename std::vector<AppendVectorNS::AppendVector<OO>>::iterator objects,
	const size_t size,
	std::vector<std::shared_ptr<TileCoordinatesSet>>& zooms
) {
	for (size_t i = 0; i < size; i++)
		collectTilesWithObjectsInShardTemplate<OO>(indexZoom, objects[i], i, zooms);
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
	const typename std::vector<std::vector<OO>>& objects,
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

// The order the shards are sorted in (see sortOutputObjects) as a number: x and y (offsets in the
// z6 tile, below 2^(indexZoom - CLUSTER_ZOOM)) bit-interleaved, x above y at each level.
inline uint32_t indexSpreadBits(uint32_t v) {
	v &= 0x0000FFFF;
	v = (v | (v << 8)) & 0x00FF00FF;
	v = (v | (v << 4)) & 0x0F0F0F0F;
	v = (v | (v << 2)) & 0x33333333;
	v = (v | (v << 1)) & 0x55555555;
	return v;
}
inline uint32_t indexOrderKey(uint32_t x, uint32_t y) {
	return (indexSpreadBits(x) << 1) | indexSpreadBits(y);
}

// When non-zero, getObjectsForTile keeps the objects shown up to this zoom rather than the tile's
// own zoom (see the covered subtrees in tilemaker.cpp)
extern thread_local unsigned objectsUpToZoom;

template<typename OO> void collectObjectsForTileTemplate(
	const unsigned int& indexZoom,
	typename std::vector<AppendVectorNS::AppendVector<OO>>::iterator objects,
	size_t iStart,
	size_t iEnd,
	unsigned int zoom,
	TileCoordinates dstIndex,
	std::vector<OutputObjectID>& output,
	bool* nonPolygon = nullptr	// if given: stop at the first object that isn't a polygon, and set it
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

		// (At the index zoom itself, one index tile: the search below is as fast)
		if (bz >= CLUSTER_ZOOM && bz - CLUSTER_ZOOM <= 8 * sizeof(Z6Offset) && clampedZoom < bz) {
			// The shard is sorted by (x, y) compared at each zoom from CLUSTER_ZOOM to indexZoom, i.e. by
			// indexOrderKey (x, y < 2^(indexZoom - CLUSTER_ZOOM)). The index tiles inside the tile are those
			// with keys [key(needle), key(needle) + 4^(indexZoom - clampedZoom)): one range, found with
			// binary searches on the key. The same objects, in the same order, as the loop below.
			const uint32_t first = indexOrderKey(needleX, needleY);
			const uint32_t last = first + (uint32_t(1) << (2 * (bz - clampedZoom)));
			auto keyLess = [](const OO& a, uint32_t k) { return indexOrderKey(a.x, a.y) < k; };
			auto from = std::lower_bound(objects[i].begin(), objects[i].end(), first, keyLess);
			// Mostly a few objects (deep zooms): look at them one by one first
			auto to = from;
			unsigned steps = 0;
			while (to != objects[i].end() && steps < 32 && keyLess(*to, last)) { ++to; ++steps; }
			if (steps == 32) to = std::lower_bound(to, objects[i].end(), last, keyLess);
			const unsigned upTo = objectsUpToZoom ? objectsUpToZoom : zoom;
			for (; from != to; ++from) {
				if (from->oo.minZoom <= upTo) {
					if (nonPolygon && from->oo.geomType != POLYGON_) { *nonPolygon = true; return; }
					output.push_back(outputObjectWithId(*from));
				}
			}
			continue;
		}

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
				if (iter->oo.minZoom <= (objectsUpToZoom ? objectsUpToZoom : zoom)) {
					if (nonPolygon && iter->oo.geomType != POLYGON_) { *nonPolygon = true; return; }
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
	using point_store_t = AppendVectorNS::AppendVector<Point>;

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
	// Per multipolygonStores entry: stored geometry is known to pass geom::is_valid
	std::vector<std::vector<bool>> multipolygonValid;
	// Validity checks still running (see checkValidityLater)
	std::mutex deferredValidityMutex;
	std::vector<std::pair<NodeID, std::future<bool>>> deferredValidity;
	
	ClipCache<MultiPolygon> multiPolygonClipCache;
	ClipCache<MultiLinestring> multiLinestringClipCache;

	// Small index adds are staged per thread and moved to their shards in bulk (see flushStaging)
	template<class OO> struct IndexStaging {
		std::vector<OO> entries;
		std::vector<uint16_t> shards;	// z6 index of each entry
	};
	struct alignas(64) IndexStagings {	// a cache line of its own: each is written by its thread only
		IndexStaging<OutputObjectXY> objects;
		IndexStaging<OutputObjectXYID> objectsWithIds;
	};
	std::mutex stagingMutex;
	std::deque<IndexStagings> stagings;	// one per thread that added objects (guarded by stagingMutex)
	uint64_t stagingEpoch;	// identifies this source's stagings in threads' caches (renewed by finalize)

public:
	TileDataSource(size_t threadNum, unsigned int indexZoom, bool includeID);
	~TileDataSource();
	const uint64_t leaseNumber = registerLeaseSource(this);	// see registerLeaseSource

	void collectTilesWithObjectsAtZoom(std::vector<std::shared_ptr<TileCoordinatesSet>>& zooms);
	// The same for the objects of one z6 tile (0 <= shard < CLUSTER_ZOOM_AREA)
	void collectTilesWithObjectsInShard(size_t shard, std::vector<std::shared_ptr<TileCoordinatesSet>>& zooms);

	void collectTilesWithLargeObjectsAtZoom(std::vector<std::shared_ptr<TileCoordinatesSet>>& zooms);

	// nonPolygon: see collectObjectsForTileTemplate (z6 and above)
	void collectObjectsForTile(uint zoom, TileCoordinates dstIndex, std::vector<OutputObjectID>& output, bool* nonPolygon = nullptr);
	// Whether the objects getObjectsForTile(zoom, dstIndex) returns are none (0), all polygons (1),
	// or not (2), without sorting them: for the covered-subtree check
	int objectKindsForTile(uint zoom, TileCoordinates dstIndex);
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
	// As addObjectToSmallIndex(tile, oo, id) for each tile and the outputs each(tile, add) calls
	// add(oo) for, taking a z6 tile's lock once per run of tiles in it
	template<class Each> void addToSmallIndex(const std::vector<TileCoordinates>& tiles, uint64_t id, Each each);
	IndexStagings& indexStaging();
	void stageObject(IndexStagings& staging, size_t z6index, const TileCoordinates& index, const OutputObject& oo, uint64_t id);
	void flushStagingIfFull(IndexStagings& staging);
	template<class OO> void flushStaging(IndexStaging<OO>& staging, std::vector<AppendVectorNS::AppendVector<OO>>& shards);

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
		const AttributeStore& attributeStore,
		unsigned int zoom,
		TileCoordinates coordinates
	);
	void orderStoredGeometryTies(std::vector<OutputObjectID>& data) const;

	virtual Geometry buildWayGeometry(OutputGeometryType const geomType, NodeID const objectID, const TileBbox &bbox);

	// Large polygons are clipped once per SHARED_PIECE_ZOOM tile before the tiles are written,
	// and deeper tiles clip from that piece instead of the whole polygon (see tile_data.cpp).
	static constexpr unsigned SHARED_PIECE_ZOOM = 8;
	// present(x, y): whether any tile below SHARED_PIECE_ZOOM tile x/y will be written
	void precomputeSharedPieces(const std::function<bool(TileCoordinate, TileCoordinate)> &present, unsigned threadNum);
	// Compute pieces in the order of their z6 tiles (x * 64 + y, the order the tiles are written in):
	// those of z6 tiles up to handedOutZ6, and those after it while the pieces alive take less than
	// budgetBytes. The tile threads call this after taking a batch.
	void computeSharedPieces(int64_t handedOutZ6, uint64_t budgetBytes);
	// Drop the shared pieces inside a z6 tile, once no tile in it is still to be written
	void releaseSharedPieces(TileCoordinate z6x, TileCoordinate z6y);
	// Whether objectID's polygon is kept in the multipolygon store (retrieveMultiPolygon)
	virtual bool storesMultiPolygon(NodeID objectID) const { return true; }
	struct SharedPiece {
		std::shared_ptr<MultiPolygon> geom;	// null: no usable piece, clip the whole polygon
		bool valid = false;
		uint64_t bytes = 0;	// counted in livePieceBytes while the piece is alive
		~SharedPiece() { livePieceBytes -= bytes; }
	};
	// The points of all pieces alive, of all sources
	static std::atomic<uint64_t> livePieceBytes;
	// Filled by precomputeSharedPieces, then only read while the tiles are written (no lock).
	// releaseSharedPieces resets the pieces of a z6 tile no thread reads any more.
	std::unordered_set<NodeID> pieceObjects;
	std::map<std::tuple<NodeID, TileCoordinate, TileCoordinate>, std::shared_future<std::shared_ptr<const SharedPiece>>> sharedPieces;
	struct PieceJob {
		std::tuple<NodeID, TileCoordinate, TileCoordinate> key;
		uint64_t points;
		std::shared_ptr<std::promise<std::shared_ptr<const SharedPiece>>> promise;
		int64_t z6;
	};
	std::vector<PieceJob> pieceJobs;	// by z6 tile, largest polygon first
	std::atomic<size_t> nextPieceJob { 0 };
	// Computes job i
	void computeSharedPiece(size_t i);
	virtual LatpLon buildNodeGeometry(NodeID const objectID, const TileBbox &bbox) const;

	void open() {
		// Put something at index 0 of all stores so that 0 can be used
		// as a sentinel.
		pointStores[0].push_back(Point(0,0));
		linestringStores[0].push_back(linestring_t());
		multipolygonStores[0].push_back(multi_polygon_t());
		multipolygonValid[0].push_back(false);
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
	// True if the polygon populateMultiPolygon returns for objectID is known to
	// have passed geom::is_valid on exactly the same coordinates.
	virtual bool polygonKnownValid(NodeID objectID) const {
		const auto& valid = multipolygonValid[getShard(objectID)];
		const auto offset = getId(objectID);
		return offset < valid.size() && valid[offset];
	}

	unsigned int getIndexZoom() const { return indexZoom; }
	inline size_t getId(NodeID id) const {
		return id & (~(~0ull << (TILE_DATA_ID_SIZE - shardBits)));
	}

	const Point& retrievePoint(NodeID id) const {
		const auto& shardId = getShard(id);
		const auto& shard = pointStores[shardId];
		const auto offset = getId(id);
		if (offset >= shard.size()) throw std::out_of_range("Could not find generated node with id " + std::to_string(id) + ", shard " + std::to_string(shardId) + ", offset=" + std::to_string(offset));
		return shard[offset];
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

	// knownValid: src has passed geom::is_valid (see polygonKnownValid)
	NodeID storeMultiPolygon(const MultiPolygon& src, bool knownValid = false);
	// Find out in the background whether a polygon stored with knownValid=false passes
	// geom::is_valid: on a big lake with thousands of islands that takes ~10 s, which would
	// otherwise hold up the end of reading. finishDeferredValidity records the results.
	void checkValidityLater(NodeID objectID, MultiPolygon mp);
	void finishDeferredValidity();

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
	std::vector<std::shared_ptr<TileCoordinatesSet>>& zooms,
	size_t threadNum = 1
);

#endif //_TILE_DATA_H
