#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <sstream>
#include <type_traits>
#include "tile_data.h"
#include <tuple>
#include "coordinates_geom.h"
#include "leased_store.h"
#include <unordered_map>
#include "stable_hash.h"
#include "clip_line.h"

thread_local unsigned objectsUpToZoom = 0;
std::vector<uint32_t> z12ObjectCounts;
#include <ciso646>

using namespace std;
extern bool verbose;

namespace {
// Each thread's staging for the source it last added to; epochs are unique across sources
std::atomic<uint64_t> nextStagingEpoch(1);
thread_local uint64_t tlsStagingEpoch = 0;
thread_local void* tlsStaging = nullptr;	// TileDataSource::IndexStagings
// Entries staged per thread before they are moved to the shards
const size_t INDEX_STAGING_SIZE = 1024;
}

// Human-readable name for a Boost.Geometry validity failure, used for diagnostics.
static const char* validityFailureName(geom::validity_failure_type failure) {
	switch (failure) {
		case geom::no_failure:                        return "no_failure";
		case geom::failure_few_points:                return "few_points";
		case geom::failure_wrong_topological_dimension: return "wrong_topological_dimension";
		case geom::failure_spikes:                    return "spikes";
		case geom::failure_duplicate_points:          return "duplicate_points";
		case geom::failure_not_closed:                return "not_closed";
		case geom::failure_self_intersections:        return "self_intersections";
		case geom::failure_wrong_orientation:         return "wrong_orientation";
		case geom::failure_interior_rings_outside:    return "interior_rings_outside";
		case geom::failure_nested_interior_rings:     return "nested_interior_rings";
		case geom::failure_disconnected_interior:     return "disconnected_interior";
		case geom::failure_intersecting_interiors:    return "intersecting_interiors";
		case geom::failure_wrong_corner_order:        return "wrong_corner_order";
		case geom::failure_invalid_coordinate:        return "invalid_coordinate";
		default:                                      return "unknown";
	}
}

// Thin wrapper around the shared repair_multi_polygon() that adds per-object
// verbose diagnostics. See geom.cpp for the dissolve + zero-width buffer logic.
static bool repairMultiPolygon(MultiPolygon &mp, NodeID objectID, bool *wholeValid = nullptr) {
	bool ok = repair_multi_polygon(mp, false, wholeValid);
	if (!ok && verbose)
		std::cerr << ("multipolygon repair failed for object " + std::to_string(objectID) + "\n");
	return ok;
}

thread_local LeasedStore<TileDataSource::point_store_t> pointStore;
thread_local LeasedStore<TileDataSource::linestring_store_t> linestringStore;
thread_local LeasedStore<TileDataSource::multi_linestring_store_t> multilinestringStore;
thread_local LeasedStore<TileDataSource::multi_polygon_store_t> multipolygonStore;

// Never destroyed: the thread-locals that use them may be destroyed after the statics (macOS)
static std::unordered_map<const TileDataSource *, uint64_t> &leaseSources() {
	static std::unordered_map<const TileDataSource *, uint64_t> &sources = *new std::unordered_map<const TileDataSource *, uint64_t>();
	return sources;
}
std::mutex &leaseSourcesMutex() {
	static std::mutex &m = *new std::mutex;
	return m;
}
uint64_t registerLeaseSource(const TileDataSource *source) {
	static uint64_t last = 0;
	std::lock_guard<std::mutex> lock(leaseSourcesMutex());
	const uint64_t number = ++last;
	leaseSources()[source] = number;
	return number;
}
void unregisterLeaseSource(const TileDataSource *source) {
	std::lock_guard<std::mutex> lock(leaseSourcesMutex());
	leaseSources().erase(source);
}
bool leaseSourceAlive(const TileDataSource *source, uint64_t number) {
	const auto it = leaseSources().find(source);
	return it != leaseSources().end() && it->second == number;
}
TileDataSource::~TileDataSource() {
	unregisterLeaseSource(this);
}

TileDataSource::TileDataSource(size_t threadNum, unsigned int indexZoom, bool includeID)
	:
	includeID(includeID),
	z6OffsetDivisor(indexZoom >= CLUSTER_ZOOM ? (1 << (indexZoom - CLUSTER_ZOOM)) : 1),
	objectsMutex(threadNum * 4),
	stagingEpoch(0),
	objects(CLUSTER_ZOOM_AREA),
	lowZoomObjects(CLUSTER_ZOOM_AREA),
	objectsWithIds(CLUSTER_ZOOM_AREA),
	lowZoomObjectsWithIds(CLUSTER_ZOOM_AREA),
	indexZoom(indexZoom),
	pointStores(threadNum),
	linestringStores(threadNum),
	multilinestringStores(threadNum),
	multipolygonStores(threadNum),
	multipolygonValid(threadNum),
	multiPolygonClipCache(ClipCache<MultiPolygon>(threadNum, indexZoom)),
	multiLinestringClipCache(ClipCache<MultiLinestring>(threadNum, indexZoom))
{
	// TileDataSource can only index up to zoom 14. The caller is responsible for
	// ensuring it does not use a higher zoom.
	if (indexZoom > 14)
		throw std::out_of_range("TileDataSource: indexZoom cannot be higher than 14, but was " + std::to_string(indexZoom));


	stagingEpoch = nextStagingEpoch++;

	shardBits = 0;
	numShards = 1;
	while(numShards < threadNum) {
		shardBits++;
		numShards *= 2;
	}

	for (int i = 0; i < threadNum; i++) {
		availablePointStoreLeases.push_back(std::make_pair(i, &pointStores[i]));
		availableLinestringStoreLeases.push_back(std::make_pair(i, &linestringStores[i]));
		availableMultiLinestringStoreLeases.push_back(std::make_pair(i, &multilinestringStores[i]));
		availableMultiPolygonStoreLeases.push_back(std::make_pair(i, &multipolygonStores[i]));
	}
}

thread_local uint64_t clipCacheBatchId = 0;
thread_local bool clipCacheNoStore = false;
thread_local bool clipCacheBypass = false;
thread_local bool clipNeededRepair = false;	// set when a POLYGON_ clip went down the repair path
thread_local bool clipResultValid = false;	// whether the last POLYGON_ clip is known to be valid

// Shared pieces. Every tile batch that needs a large polygon (often 100,000+ points) used to copy
// it from the store and clip it from scratch, at every zoom. Instead, polygons with at least
// SHARED_PIECE_MIN_POINTS points are clipped once per SHARED_PIECE_ZOOM tile before the tiles are
// written, and tiles below clip from that piece. A piece only depends on the polygon and the tile,
// never on the thread or the order, so the output stays the same for any thread count.
//
// The pieces are known up front and computed by all threads ahead of the tiles that need them:
// computed on demand, a slow piece (up to ~15 s on the planet) held up every thread whose tiles
// needed it, a third of the tile phase on a 96-thread planet run. They are computed in the order
// of their z6 tiles, the order the tiles are written in, as far ahead as a memory budget allows
// (all of them at the start on a large machine; all at once, a planet's took about 5 GB). Only a
// tile that needs a piece still being computed waits for it, and one that needs a piece not
// started yet (a slow group handed out ahead of its place) computes it. The map itself doesn't
// change, so the tiles look pieces up without a lock.
#define SHARED_PIECE_MIN_POINTS 10000

std::atomic<uint64_t> TileDataSource::livePieceBytes(0);

void TileDataSource::precomputeSharedPieces(const std::function<bool(TileCoordinate, TileCoordinate)> &present, unsigned threadNum) {
	const unsigned zoom = SHARED_PIECE_ZOOM;
	if (zoom > indexZoom) return;
	const unsigned shift = indexZoom - zoom;
	// Candidates: polygons in the large-object index (those spanning 16+ index tiles), with the
	// index tiles they cover
	std::map<NodeID, Box> boxes;
	auto add = [&](const Box &box, const OutputObject &oo) {
		if (oo.geomType != POLYGON_ || !storesMultiPolygon(oo.objectID)) return;
		auto it = boxes.find(oo.objectID);
		if (it == boxes.end()) boxes.emplace(oo.objectID, box);
		else boost::geometry::expand(it->second, box);
	};
	for (auto const &r : boxRtree) add(r.first, r.second);
	for (auto const &r : boxRtreeWithIds) add(r.first, r.second.oo);
	if (boxes.empty()) return;
	const std::vector<std::pair<NodeID, Box>> candidates(boxes.begin(), boxes.end());

	// The pieces each candidate needs: tiles below which a tile is written, that its envelope reaches
	using Job = PieceJob;
	std::vector<Job> jobs;
	std::mutex jobsMutex;
	std::atomic<size_t> next(0);
	auto findJobs = [&]() {
		std::vector<Job> local;
		for (size_t i = next++; i < candidates.size(); i = next++) {
			const NodeID objectID = candidates[i].first;
			uint64_t points = 0;
			Box envelope;
			boost::geometry::assign_inverse(envelope);
			for (auto const &p : retrieveMultiPolygon(objectID)) {
				points += p.outer().size();
				for (auto const &inner : p.inners()) points += inner.size();
				for (auto const &pt : p.outer()) boost::geometry::expand(envelope, pt);
			}
			if (points < SHARED_PIECE_MIN_POINTS) continue;
			const Box &box = candidates[i].second;
			const TileCoordinate x0 = (TileCoordinate)box.min_corner().x() >> shift, x1 = (TileCoordinate)box.max_corner().x() >> shift;
			const TileCoordinate y0 = (TileCoordinate)box.min_corner().y() >> shift, y1 = (TileCoordinate)box.max_corner().y() >> shift;
			for (TileCoordinate x = x0; x <= x1; x++)
				for (TileCoordinate y = y0; y <= y1; y++)
					if (present(x, y) && boost::geometry::intersects(envelope, TileBbox(TileCoordinates(x, y), zoom, false, false).clippingBox))
						local.push_back(Job { std::make_tuple(objectID, x, y), points, nullptr,
							(int64_t)(x >> (zoom - CLUSTER_ZOOM)) * CLUSTER_ZOOM_WIDTH + (y >> (zoom - CLUSTER_ZOOM)) });
		}
		std::lock_guard<std::mutex> lock(jobsMutex);
		jobs.insert(jobs.end(), local.begin(), local.end());
	};
	{
		boost::asio::thread_pool pool(threadNum);
		for (unsigned t = 0; t < threadNum; t++) boost::asio::post(pool, findJobs);
		pool.join();
	}
	std::sort(jobs.begin(), jobs.end(), [](const Job &a, const Job &b) {
		return a.z6 != b.z6 ? a.z6 < b.z6 : a.points != b.points ? a.points > b.points : a.key < b.key;
	});
	for (size_t i = 0; i < jobs.size(); i++) {
		Job &job = jobs[i];
		pieceObjects.insert(std::get<0>(job.key));
		job.promise = std::make_shared<std::promise<std::shared_ptr<const SharedPiece>>>();
		sharedPieces.emplace(job.key, PieceSlot { job.promise->get_future().share(), i });
	}
	pieceStarted.reset(new std::atomic<bool>[jobs.size()]());
	pieceJobs = std::move(jobs);
	nextPieceJob = 0;
}

void TileDataSource::computeSharedPieces(int64_t handedOutZ6, uint64_t budgetBytes) {
	for (size_t i = nextPieceJob.load(); i < pieceJobs.size(); i = nextPieceJob.load()) {
		if (pieceJobs[i].z6 > handedOutZ6 && livePieceBytes.load(std::memory_order_relaxed) >= budgetBytes) return;
		if (nextPieceJob.compare_exchange_weak(i, i + 1)) computeSharedPiece(i);
	}
}

void TileDataSource::computeSharedPiece(size_t i) {
	const unsigned zoom = SHARED_PIECE_ZOOM;
	if (pieceStarted[i].load(std::memory_order_relaxed) || pieceStarted[i].exchange(true)) return;
	const auto &key = pieceJobs[i].key;
	auto piece = std::make_shared<SharedPiece>();
	// (also called from a tile's buildWayGeometry: see the POLYGON_ case)
	const bool bypass = clipCacheBypass;
	try {
		clipCacheBypass = true;
		clipNeededRepair = false;
		Geometry g = buildWayGeometry(POLYGON_, std::get<0>(key), TileBbox(TileCoordinates(std::get<1>(key), std::get<2>(key)), zoom, false, false));
		// A piece that needed the repair path can be wrong (once a lake came out empty), and
		// every tile below it would inherit that: leave those tiles to clip the whole polygon.
		if (!clipNeededRepair) {
			piece->geom = std::make_shared<MultiPolygon>(std::move(boost::get<MultiPolygon>(g)));
			// buildWayGeometry has just checked it (for a huge piece, is_valid takes seconds)
			piece->valid = clipResultValid;
			for (const auto &polygon : *piece->geom) {
				piece->bytes += polygon.outer().size() * sizeof(Point);
				for (const auto &inner : polygon.inners()) piece->bytes += inner.size() * sizeof(Point);
			}
			livePieceBytes += piece->bytes;
		}
	} catch (std::exception &) {
		// The tiles clip the whole polygon, and report the problem there
	}
	clipCacheBypass = bypass;
	pieceJobs[i].promise->set_value(piece);
	pieceJobs[i].promise.reset();	// the map's future keeps the piece, until its z6 tile is released
}

uint64_t TileDataSource::releaseSharedPieces(TileCoordinate z6x, TileCoordinate z6y) {
	const unsigned shift = SHARED_PIECE_ZOOM - CLUSTER_ZOOM;
	uint64_t bytes = 0;
	for (auto &e : sharedPieces) {
		std::shared_future<std::shared_ptr<const SharedPiece>> &future = e.second.piece;
		if ((std::get<1>(e.first) >> shift) != z6x || (std::get<2>(e.first) >> shift) != z6y || !future.valid()) continue;
		// (a piece no tile needed may still be being computed)
		if (future.wait_for(std::chrono::seconds(0)) == std::future_status::ready && future.get())
			bytes += future.get()->bytes;
		future = std::shared_future<std::shared_ptr<const SharedPiece>>();
	}
	return bytes;
}

void TileDataSource::finalize(size_t threadNum) {
	// The objects the threads have staged last (at most INDEX_STAGING_SIZE + one object's tiles per
	// thread). The order within a shard doesn't matter: shards are sorted below.
	// Nothing adds to the small index after finalize (shapefiles, GeoJSON, Lua and declutter all
	// run before). Renewing the epoch makes threads' cached staging pointers stale.
	uint64_t finalized = 0;
	for (IndexStagings& staging : stagings) {
		finalized += staging.objects.entries.size() + staging.objectsWithIds.entries.size();
		flushStaging(staging.objects, objects);
		flushStaging(staging.objectsWithIds, objectsWithIds);
	}
	std::deque<IndexStagings>().swap(stagings);
	stagingEpoch = nextStagingEpoch++;

	std::cout << "indexed " << finalized << " staged objects" << std::endl;

	finalizeObjects<OutputObjectXY>(name(), threadNum, indexZoom, objects.begin(), objects.end(), lowZoomObjects);
	finalizeObjects<OutputObjectXYID>(name(), threadNum, indexZoom, objectsWithIds.begin(), objectsWithIds.end(), lowZoomObjectsWithIds);
}

void TileDataSource::addObjectToSmallIndex(const TileCoordinates& index, const OutputObject& oo, uint64_t id) {
	// Pick the z6 index
	const size_t z6x = index.x / z6OffsetDivisor;
	const size_t z6y = index.y / z6OffsetDivisor;

	if (z6x >= 64 || z6y >= 64) {
		if (verbose) std::cerr << "ignoring OutputObject with invalid z" << indexZoom << " coordinates " << index.x << ", " << index.y << " (id: " << id << ")" << std::endl;
		return;
	}

	const size_t z6index = z6x * CLUSTER_ZOOM_WIDTH + z6y;
	IndexStagings& staging = indexStaging();
	stageObject(staging, z6index, index, oo, id);
	flushStagingIfFull(staging);
}

TileDataSource::IndexStagings& TileDataSource::indexStaging() {
	if (tlsStagingEpoch != stagingEpoch) {
		std::lock_guard<std::mutex> lock(stagingMutex);
		stagings.emplace_back();
		if (!includeID) stagings.back().objects.entries.reserve(INDEX_STAGING_SIZE + 64);
		tlsStaging = &stagings.back();
		tlsStagingEpoch = stagingEpoch;
	}
	return *static_cast<IndexStagings*>(tlsStaging);
}

void TileDataSource::stageObject(IndexStagings& staging, size_t z6index, const TileCoordinates& index, const OutputObject& oo, uint64_t id) {
	const size_t z6x = z6index / CLUSTER_ZOOM_WIDTH, z6y = z6index % CLUSTER_ZOOM_WIDTH;
	const Z6Offset x = index.x - (z6x * z6OffsetDivisor), y = index.y - (z6y * z6OffsetDivisor);
	if (id == 0 || !includeID) {
		staging.objects.entries.push_back({ oo, x, y });
		staging.objects.shards.push_back(z6index);
	} else {
		staging.objectsWithIds.entries.push_back({ oo, x, y, id });
		staging.objectsWithIds.shards.push_back(z6index);
	}
}

void TileDataSource::flushStagingIfFull(IndexStagings& staging) {
	if (staging.objects.entries.size() >= INDEX_STAGING_SIZE) flushStaging(staging.objects, objects);
	if (staging.objectsWithIds.entries.size() >= INDEX_STAGING_SIZE) flushStaging(staging.objectsWithIds, objectsWithIds);
}

// Moves the staged entries to their shards: grouped by shard (a counting sort), then one lock and
// one bulk append per shard, instead of a lock per object (and z6 run). Shards whose lock is busy
// are done after the others, then waiting for the lock: its holder only appends.
template<class OO>
void TileDataSource::flushStaging(IndexStaging<OO>& staging, std::vector<AppendVectorNS::AppendVector<OO>>& shards) {
	const size_t n = staging.entries.size();
	if (n == 0) return;
	const uint16_t* z6 = staging.shards.data();
	size_t same = 1;
	while (same < n && z6[same] == z6[0]) same++;
	if (same == n) {
		std::lock_guard<std::mutex> lock(objectsMutex[z6[0] % objectsMutex.size()]);
		shards[z6[0]].append(staging.entries.data(), n);
	} else {
		static_assert(std::is_trivially_copyable<OO>::value && sizeof(OO) % sizeof(uint64_t) == 0, "staged as raw words");
		thread_local std::vector<uint32_t> counts(CLUSTER_ZOOM_AREA, 0);
		thread_local std::vector<uint16_t> touched;
		thread_local std::vector<uint32_t> begins;
		thread_local std::vector<size_t> busy;
		thread_local std::vector<uint64_t> sortedWords;	// OO isn't default constructible
		touched.clear(); busy.clear();
		for (size_t k = 0; k < n; k++)
			if (counts[z6[k]]++ == 0) touched.push_back(z6[k]);
		begins.resize(touched.size() + 1);
		uint32_t offset = 0;
		for (size_t t = 0; t < touched.size(); t++) {
			begins[t] = offset;
			const uint32_t c = counts[touched[t]];
			counts[touched[t]] = offset;
			offset += c;
		}
		begins[touched.size()] = offset;
		sortedWords.resize(n * sizeof(OO) / sizeof(uint64_t));
		OO* sorted = reinterpret_cast<OO*>(sortedWords.data());
		for (size_t k = 0; k < n; k++)
			std::memcpy(static_cast<void*>(&sorted[counts[z6[k]]++]), &staging.entries[k], sizeof(OO));
		for (size_t t = 0; t < touched.size(); t++) {
			counts[touched[t]] = 0;
			std::unique_lock<std::mutex> lock(objectsMutex[touched[t] % objectsMutex.size()], std::try_to_lock);
			if (lock.owns_lock()) shards[touched[t]].append(sorted + begins[t], begins[t + 1] - begins[t]);
			else busy.push_back(t);
		}
		for (size_t t : busy) {
			std::lock_guard<std::mutex> lock(objectsMutex[touched[t] % objectsMutex.size()]);
			shards[touched[t]].append(sorted + begins[t], begins[t + 1] - begins[t]);
		}
		if (sortedWords.capacity() > 16 * INDEX_STAGING_SIZE * sizeof(OO) / sizeof(uint64_t)) std::vector<uint64_t>().swap(sortedWords);
	}
	staging.entries.clear();
	staging.shards.clear();
	// one huge object (a long line) can leave a big buffer
	if (staging.entries.capacity() > 16 * INDEX_STAGING_SIZE) {
		std::vector<OO>().swap(staging.entries);
		std::vector<uint16_t>().swap(staging.shards);
		staging.entries.reserve(INDEX_STAGING_SIZE + 64);
	}
}

template<class Each>
void TileDataSource::addToSmallIndex(const std::vector<TileCoordinates>& tiles, uint64_t id, Each each) {
	IndexStagings& staging = indexStaging();
	for (const TileCoordinates& tile : tiles) {
		const size_t z6x = tile.x / z6OffsetDivisor;
		const size_t z6y = tile.y / z6OffsetDivisor;
		if (z6x >= 64 || z6y >= 64) {
			// addObjectToSmallIndex reports and ignores it
			each(tile, [&](const OutputObject& oo) { addObjectToSmallIndex(tile, oo, id); });
			continue;
		}
		const size_t z6index = z6x * CLUSTER_ZOOM_WIDTH + z6y;
		each(tile, [&](const OutputObject& oo) { stageObject(staging, z6index, tile, oo, id); });
	}
	flushStagingIfFull(staging);
}

void TileDataSource::addObjectToSmallIndexUnsafe(const TileCoordinates& index, const OutputObject& oo, uint64_t id) {
	// Pick the z6 index
	const size_t z6x = index.x / z6OffsetDivisor;
	const size_t z6y = index.y / z6OffsetDivisor;
	const size_t z6index = z6x * CLUSTER_ZOOM_WIDTH + z6y;

	if (id == 0 || !includeID)
		objects[z6index].push_back({
			oo,
			(Z6Offset)(index.x - (z6x * z6OffsetDivisor)),
			(Z6Offset)(index.y - (z6y * z6OffsetDivisor))
		});
	else
		objectsWithIds[z6index].push_back({
			oo,
			(Z6Offset)(index.x - (z6x * z6OffsetDivisor)),
			(Z6Offset)(index.y - (z6y * z6OffsetDivisor)),
			id
		});
}

void TileDataSource::collectTilesWithObjectsAtZoom(std::vector<std::shared_ptr<TileCoordinatesSet>>& zooms) {
	// Scan through all shards. Convert to base zoom, then convert to the requested zoom.
	collectTilesWithObjectsAtZoomTemplate<OutputObjectXY>(indexZoom, objects.begin(), objects.size(), zooms);
	collectTilesWithObjectsAtZoomTemplate<OutputObjectXYID>(indexZoom, objectsWithIds.begin(), objectsWithIds.size(), zooms);
}

void TileDataSource::collectTilesWithObjectsInShard(size_t shard, std::vector<std::shared_ptr<TileCoordinatesSet>>& zooms) {
	if (shard < objects.size())
		collectTilesWithObjectsInShardTemplate<OutputObjectXY>(indexZoom, objects[shard], shard, zooms);
	if (shard < objectsWithIds.size())
		collectTilesWithObjectsInShardTemplate<OutputObjectXYID>(indexZoom, objectsWithIds[shard], shard, zooms);
}

void addCoveredTilesToOutput(const uint indexZoom, std::vector<std::shared_ptr<TileCoordinatesSet>>& zooms, const Box& box) {
	size_t maxZoom = zooms.size() - 1;
// 	std::cout << "addCoveredTilesToOutput maxZoom=" << maxZoom << ", indexZoom - maxZoom = " << (indexZoom - maxZoom) << std::endl;
	int scale = pow(2, indexZoom - maxZoom);
	TileCoordinate minx = box.min_corner().x() / scale;
	TileCoordinate maxx = box.max_corner().x() / scale;
	TileCoordinate miny = box.min_corner().y() / scale;
	TileCoordinate maxy = box.max_corner().y() / scale;
	// The parents of the tiles in the box are the tiles in the box scaled down
	for (int zoom = maxZoom; zoom >= 0; zoom--) {
		zooms[zoom]->setBox(minx, maxx, miny, maxy);
		minx /= 2; maxx /= 2;
		miny /= 2; maxy /= 2;
	}
}

// Find the tiles used by the "large objects" from the rtree index
void TileDataSource::collectTilesWithLargeObjectsAtZoom(std::vector<std::shared_ptr<TileCoordinatesSet>>& zooms) {
	for(auto const &result: boxRtree)
		addCoveredTilesToOutput(indexZoom, zooms, result.first);

	for(auto const &result: boxRtreeWithIds)
		addCoveredTilesToOutput(indexZoom, zooms, result.first);
}

// Copy objects from the tile at dstIndex (in the dataset srcTiles) into output
void TileDataSource::collectObjectsForTile(
	uint zoom,
	TileCoordinates dstIndex,
	std::vector<OutputObjectID>& output,
	bool* nonPolygon
) {
	if (zoom < CLUSTER_ZOOM) {
		collectLowZoomObjectsForTile<OutputObjectXY>(indexZoom, lowZoomObjects, zoom, dstIndex, output);
		collectLowZoomObjectsForTile<OutputObjectXYID>(indexZoom, lowZoomObjectsWithIds, zoom, dstIndex, output);
		return;
	}

	size_t iStart = 0;
	size_t iEnd = objects.size();

	if (zoom >= CLUSTER_ZOOM) {
		TileCoordinate z6x = dstIndex.x / (1 << (zoom - CLUSTER_ZOOM));
		TileCoordinate z6y = dstIndex.y / (1 << (zoom - CLUSTER_ZOOM));

		if (z6x >= 64 || z6y >= 64) {
			if (verbose) std::cerr << "collectObjectsForTile: invalid tile z" << zoom << "/" << dstIndex.x << "/" << dstIndex.y << std::endl;
			return;
		}
		iStart = z6x * CLUSTER_ZOOM_WIDTH + z6y;
		iEnd = iStart + 1;
	}

	collectObjectsForTileTemplate<OutputObjectXY>(indexZoom, objects.begin(), iStart, iEnd, zoom, dstIndex, output, nonPolygon);
	if (nonPolygon && *nonPolygon) return;
	collectObjectsForTileTemplate<OutputObjectXYID>(indexZoom, objectsWithIds.begin(), iStart, iEnd, zoom, dstIndex, output, nonPolygon);
}

int TileDataSource::objectKindsForTile(uint zoom, TileCoordinates dstIndex) {
	// The objects collected are those getObjectsForTile sorts, deduplicates and orders: the same
	// kinds, and some object if any
	thread_local std::vector<OutputObjectID> collected;
	collected.clear();
	bool nonPolygon = false;
	collectObjectsForTile(zoom, dstIndex, collected, &nonPolygon);
	if (nonPolygon) return 2;
	collectLargeObjectsForTile(zoom, dstIndex, collected);
	for (const auto& o : collected)
		if (o.oo.geomType != POLYGON_) return 2;
	const bool any = !collected.empty();
	if (collected.capacity() > (1u << 16)) std::vector<OutputObjectID>().swap(collected);
	return any ? 1 : 0;
}

namespace {
// The large objects of the tiles the thread is working in, from LARGE_OBJECT_CACHE_ZOOM down: the
// tiles of a LARGE_OBJECT_CACHE_ZOOM tile come one after another, depth first. Per zoom, the objects
// that intersect the current tile at that zoom (whatever their minZoom), in the order the rtree query
// for its box returns them.
const unsigned LARGE_OBJECT_CACHE_ZOOM = 12;
struct LargeObjectLevel {
	bool valid = false;
	TileCoordinates index{0, 0};
	std::vector<std::pair<Box, OutputObject>> objects;
	std::vector<std::pair<Box, OutputObjectID>> objectsWithIds;
};
struct LargeObjectCache {
	const TileDataSource* source = nullptr;
	std::vector<LargeObjectLevel> levels;	// by zoom
};
thread_local std::vector<LargeObjectCache> tlsLargeObjects;	// one per source

Box indexBox(TileCoordinates tile, unsigned zoom, unsigned indexZoom) {
	const int scale = 1 << (indexZoom - zoom);
	return Box(geom::make<Point>(tile.x * scale, tile.y * scale),
	           geom::make<Point>((tile.x + 1) * scale - 1, (tile.y + 1) * scale - 1));
}
}

// Copy objects from the large index into output
void TileDataSource::collectLargeObjectsForTile(
	uint zoom,
	TileCoordinates dstIndex,
	std::vector<OutputObjectID>& output
) {
	unsigned int clampedZoom = zoom;
	while (clampedZoom > indexZoom) {
		clampedZoom--;
		dstIndex.x /= 2;
		dstIndex.y /= 2;
	}
	int scale = pow(2, indexZoom - clampedZoom);
	TileCoordinates srcIndex1( dstIndex.x   *scale  ,  dstIndex.y   *scale  );
	TileCoordinates srcIndex2((dstIndex.x+1)*scale-1, (dstIndex.y+1)*scale-1);
	Box box = Box(geom::make<Point>(srcIndex1.x, srcIndex1.y),
	              geom::make<Point>(srcIndex2.x, srcIndex2.y));
	const unsigned upToZoom = objectsUpToZoom ? objectsUpToZoom : zoom;
	if (clampedZoom >= LARGE_OBJECT_CACHE_ZOOM && indexZoom >= LARGE_OBJECT_CACHE_ZOOM) {
		// The rtree query visits the nodes depth first in a fixed order and returns the values that
		// intersect the box in that order. A value intersecting a box lies in nodes that intersect it,
		// and those intersect any larger box too. So the query for a tile returns the values of the
		// query for an enclosing box that intersect the tile, in the same order: each tile's objects
		// are filtered from those of its parent (from one query per LARGE_OBJECT_CACHE_ZOOM tile).
		std::vector<LargeObjectCache>& caches = tlsLargeObjects;
		size_t c = 0;
		while (c < caches.size() && caches[c].source != this) c++;
		if (c == caches.size()) { caches.emplace_back(); caches.back().source = this; }
		std::vector<LargeObjectLevel>& levels = caches[c].levels;
		if (levels.size() < clampedZoom + 1) levels.resize(clampedZoom + 1);
		// Bring the levels above this tile up to date
		for (unsigned z = LARGE_OBJECT_CACHE_ZOOM; z < clampedZoom; z++) {
			const TileCoordinates tile(dstIndex.x >> (clampedZoom - z), dstIndex.y >> (clampedZoom - z));
			LargeObjectLevel& level = levels[z];
			if (level.valid && level.index.x == tile.x && level.index.y == tile.y) continue;
			level.valid = true;
			level.index = tile;
			level.objects.clear();
			level.objectsWithIds.clear();
			const Box levelBox = indexBox(tile, z, indexZoom);
			if (z == LARGE_OBJECT_CACHE_ZOOM) {
				for (auto const& result: boxRtree | boost::geometry::index::adaptors::queried(boost::geometry::index::intersects(levelBox)))
					level.objects.push_back(result);
				for (auto const& result: boxRtreeWithIds | boost::geometry::index::adaptors::queried(boost::geometry::index::intersects(levelBox)))
					level.objectsWithIds.push_back(result);
			} else {
				for (auto const& result: levels[z - 1].objects)
					if (boost::geometry::intersects(result.first, levelBox)) level.objects.push_back(result);
				for (auto const& result: levels[z - 1].objectsWithIds)
					if (boost::geometry::intersects(result.first, levelBox)) level.objectsWithIds.push_back(result);
			}
			for (unsigned d = z + 1; d < levels.size(); d++) levels[d].valid = false;
		}
		if (clampedZoom == LARGE_OBJECT_CACHE_ZOOM) {
			// The top of the cache: this tile's own objects (its descendants filter them)
			LargeObjectLevel& level = levels[clampedZoom];
			if (!level.valid || level.index.x != dstIndex.x || level.index.y != dstIndex.y) {
				level.valid = true;
				level.index = dstIndex;
				level.objects.clear();
				level.objectsWithIds.clear();
				for (auto const& result: boxRtree | boost::geometry::index::adaptors::queried(boost::geometry::index::intersects(box)))
					level.objects.push_back(result);
				for (auto const& result: boxRtreeWithIds | boost::geometry::index::adaptors::queried(boost::geometry::index::intersects(box)))
					level.objectsWithIds.push_back(result);
				for (unsigned d = clampedZoom + 1; d < levels.size(); d++) levels[d].valid = false;
			}
			for (auto const& result: level.objects)
				if (result.second.minZoom <= upToZoom) output.push_back({result.second, 0});
			for (auto const& result: level.objectsWithIds)
				if (result.second.oo.minZoom <= upToZoom) output.push_back({result.second.oo, result.second.id});
			return;
		}
		const LargeObjectLevel& parent = levels[clampedZoom - 1];
		for (auto const& result: parent.objects)
			if (result.second.minZoom <= upToZoom && boost::geometry::intersects(result.first, box))
				output.push_back({result.second, 0});
		for (auto const& result: parent.objectsWithIds)
			if (result.second.oo.minZoom <= upToZoom && boost::geometry::intersects(result.first, box))
				output.push_back({result.second.oo, result.second.id});
		return;
	}
	for(auto const& result: boxRtree | boost::geometry::index::adaptors::queried(boost::geometry::index::intersects(box))) {
		if (result.second.minZoom <= (objectsUpToZoom ? objectsUpToZoom : zoom))
			output.push_back({result.second, 0});
	}

	for(auto const& result: boxRtreeWithIds | boost::geometry::index::adaptors::queried(boost::geometry::index::intersects(box))) {
		if (result.second.oo.minZoom <= (objectsUpToZoom ? objectsUpToZoom : zoom))
			output.push_back({result.second.oo, result.second.id});
	}
}

// Build node and way geometries
Geometry TileDataSource::buildWayGeometry(OutputGeometryType const geomType, 
                                          NodeID const objectID, const TileBbox &bbox) {
	switch(geomType) {
		case POINT_: {
			throw std::runtime_error("unexpected geomType in buildWayGeometry");
		}

		case LINESTRING_: {
			return clip_linestring_for_tile(retrieveLinestring(objectID), bbox.clippingBox, bbox.getExtendBox());
		}

		case MULTILINESTRING_: {
			// Look for a previously clipped version at z-1, z-2, ...
			std::shared_ptr<MultiLinestring> cachedClip = multiLinestringClipCache.get(bbox.zoom, bbox.index.x, bbox.index.y, objectID);

			MultiLinestring uncached;

			if (cachedClip == nullptr) {
				const auto& input = retrieveMultiLinestring(objectID);
				boost::geometry::assign(uncached, input);
			}

			const auto &mls = cachedClip == nullptr ? uncached : *cachedClip;

			// investigate whether filtering the constituent linestrings improves performance
			MultiLinestring result;
			geom::intersection(mls, bbox.getExtendBox(), result);
			multiLinestringClipCache.add(bbox, objectID, result);
			return result;
		}

		case POLYGON_: {
			// Look for a previously clipped version at z-1, z-2, ...
			auto cachedEntry = multiPolygonClipCache.getEntry(bbox.zoom, bbox.index.x, bbox.index.y, objectID);
			std::shared_ptr<MultiPolygon> cachedClip = cachedEntry.geom;
			bool inputKnownValid = cachedClip == nullptr ? polygonKnownValid(objectID) : cachedEntry.valid;
			if (cachedClip == nullptr && bbox.zoom > SHARED_PIECE_ZOOM && pieceObjects.count(objectID)) {
				// A large polygon: clip from its shared piece
				const unsigned shift = bbox.zoom - SHARED_PIECE_ZOOM;
				auto it = sharedPieces.find(std::make_tuple(objectID, bbox.index.x >> shift, bbox.index.y >> shift));
				if (it != sharedPieces.end() && it->second.piece.valid()) {
					computeSharedPiece(it->second.job);	// if no thread has started it yet
					const std::shared_ptr<const SharedPiece> &piece = it->second.piece.get();	// waits if still being computed
					if (piece && piece->geom) {
						cachedClip = piece->geom;
						inputKnownValid = piece->valid;
					}
				}
			}

			MultiPolygon uncached;

			if (cachedClip == nullptr) {
				// The cached multipolygon uses a non-standard allocator, so copy it
				populateMultiPolygon(uncached, objectID);
			}

			const auto &input = cachedClip == nullptr ? uncached : *cachedClip;

			// At the end zoom, the box takes in both ends of every segment crossing its border
			Box box = bbox.endZoom ? tile_clip_box_end_zoom(input, bbox.clippingBox, bbox.getExtendBox()) : bbox.clippingBox;

			MultiPolygon mp;
			// rect_clip below needs the input as it was before fast_clip. For a large valid
			// polygon, keep it rather than load it from the store again for every tile.
			std::size_t inputPoints = 0;
			if (cachedClip == nullptr && inputKnownValid)
				for (auto const &p : uncached) inputPoints += geom::num_points(p);
			const bool keepUncached = inputPoints > 1000;
			bool modified;
			if (cachedClip == nullptr && !keepUncached) {
				mp = std::move(uncached);
				modified = fast_clip(mp, box);
			} else
				modified = fast_clip_copy(input, box, mp);
			if (inputKnownValid && !modified) {
				// fast_clip was the identity on a geometry that already passed
				// is_valid, so correct() is a no-op and it is still valid.
				multiPolygonClipCache.add(bbox, objectID, mp, true);
				clipResultValid = true;
				return mp;
			}
			geom::correct(mp);
			geom::validity_failure_type failure = geom::validity_failure_type::no_failure;
			bool valid = modified ? multiPolygonDefinitelyValidClipped(mp, box) : multiPolygonDefinitelyValid(mp);
			if (!valid && inputKnownValid) {
				// fast_clip leaves zero-width bridges along the box edge where a concave
				// polygon leaves and re-enters the box. Clip the valid input exactly
				// instead, which is much cheaper than the is_valid/intersection/repair
				// path below; rect_clip bails out on anything it can't handle.
				MultiPolygon original;
				if (cachedClip == nullptr && !keepUncached) populateMultiPolygon(original, objectID);
				MultiPolygon clipped;
				if (rect_clip(cachedClip == nullptr && !keepUncached ? original : input, box, clipped) &&
				    (multiPolygonDefinitelyValidAfterClip(clipped) || [&] { RingPointIndexScope pointIndex; return geom::is_valid(clipped); }())) {
					multiPolygonClipCache.add(bbox, objectID, clipped, true);
					clipResultValid = true;
					return clipped;
				}
			}
			if (!valid) {
				RingPointIndexScope pointIndex;
				valid = geom::is_valid(mp,failure);
			}
			if (!valid) {
				clipNeededRepair = true;
				if (verbose) {
					// Build the whole line first and emit it with a single stream write:
					// tilemaker runs multi-threaded and chained operator<< calls are not
					// atomic, so per-token writes interleave into unreadable output.
					std::ostringstream msg;
					msg << "invalid multipolygon for object " << objectID
					    << " at z" << bbox.zoom << " " << bbox.index.x << "/" << bbox.index.y
					    << ": " << validityFailureName(failure) << "\n";
					std::cerr << msg.str();
				}
				if (failure==geom::failure_spikes) {
					geom::remove_spikes(mp);
					failure = geom::validity_failure_type::no_failure;
					valid = geom::is_valid(mp,failure);
				}
				if (!valid && (failure==geom::failure_self_intersections || failure==geom::failure_intersecting_interiors)) {
					// fast_clip can introduce self-intersections; redo the clip with the
					// slower but robust Boost intersection against the original geometry.
					MultiPolygon output;
					if (cachedClip == nullptr) {
						MultiPolygon original;
						populateMultiPolygon(original, objectID);
						geom::intersection(original, box, output);
					} else {
						geom::intersection(input, box, output);
					}
					geom::correct(output);

					// The intersection result can itself still be invalid for very complex
					// multipolygons (e.g. large reservoirs), which previously produced
					// dropped or holey tiles. Repair it before returning.
					bool outputValid;
					repairMultiPolygon(output, objectID, &outputValid);
					// Children of this tile can then take the fast paths for valid input
					multiPolygonClipCache.add(bbox, objectID, output, outputValid);
					clipResultValid = outputValid;
					return output;
				} else if (!valid) {
					// occasionally also wrong_topological_dimension, disconnected_interior:
					// defects geom::correct cannot mend. Repair mp in place; on failure it
					// is left unchanged so behaviour never regresses.
					repairMultiPolygon(mp, objectID, &valid);
				}
			}

			multiPolygonClipCache.add(bbox, objectID, mp, valid);
			clipResultValid = valid;
			return mp;
		}

		default:
			throw std::runtime_error("Invalid output geometry");
	}
}

LatpLon TileDataSource::buildNodeGeometry(NodeID const objectID, const TileBbox &bbox) const {
	auto p = retrievePoint(objectID);
	LatpLon out;
	out.latp = p.y();
	out.lon  = p.x();
	return out;
}


// Report number of stored geometries
void TileDataSource::reportSize() const {
	size_t points = 0, linestrings = 0, polygons = 0;
	for (const auto& store : pointStores)
		points += store.size();

	for (const auto& store : linestringStores)
		linestrings += store.size();

	for (const auto& store : multilinestringStores)
		linestrings += store.size();

	for (const auto& store : multipolygonStores)
		polygons += store.size();

	std::cout << "Generated points: " << (points - 1) << ", lines: " << (linestrings - 2) << ", polygons: " << (polygons - 1) << std::endl;
}

void populateTilesAtZoom(
	const std::vector<class TileDataSource *>& sources,
	std::vector<std::shared_ptr<TileCoordinatesSet>>& zooms,
	size_t threadNum
) {
	if (zooms.size() > 15)
		throw std::out_of_range("populateTilesAtZoom: expected at most z14 zooms (15), but found " + std::to_string(zooms.size()) + " vectors");

	std::vector<PreciseTileCoordinatesSet*> precise;
	for (const auto& zoom : zooms)
		precise.push_back(dynamic_cast<PreciseTileCoordinatesSet*>(zoom.get()));
	const bool allPrecise = std::find(precise.begin(), precise.end(), nullptr) == precise.end();

	if (threadNum <= 1 || !allPrecise) {
		for (size_t i = 0; i < sources.size(); i++)
			sources[i]->collectTilesWithObjectsAtZoom(zooms);
	} else {
		// Objects are kept per z6 tile. Each worker takes z6 tiles, the ones with the most
		// objects first (by columns, one column held a tenth of the planet's objects and ran on
		// alone), sets bits in bitmaps of its own, and these are merged at the end, all threads
		// merging their part of the bitmaps. A z6 tile's z12 tiles (z12ObjectCounts) are only
		// counted by the thread that takes it. The sets don't depend on who set which bit.
		std::vector<std::pair<size_t, size_t>> order;	// (objects, z6 tile)
		for (size_t shard = 0; shard < CLUSTER_ZOOM_AREA; shard++) {
			size_t n = 0;
			for (const auto source : sources) n += source->shardObjectCount(shard);
			if (n > 0) order.emplace_back(n, shard);
		}
		std::sort(order.begin(), order.end(), [](const std::pair<size_t, size_t>& a, const std::pair<size_t, size_t>& b) {
			return a.first != b.first ? a.first > b.first : a.second < b.second;
		});
		std::atomic<size_t> nextShard(0);
		std::vector<std::vector<std::shared_ptr<TileCoordinatesSet>>> local(threadNum);
		{
			boost::asio::thread_pool pool(threadNum);
			for (size_t t = 0; t < threadNum; t++) {
				boost::asio::post(pool, [&, t]() {
					auto& mine = local[t];
					for (size_t zoom = 0; zoom < zooms.size(); zoom++)
						mine.push_back(std::make_shared<PreciseTileCoordinatesSet>(zoom));
					for (size_t k; (k = nextShard++) < order.size(); )
						for (const auto source : sources)
							source->collectTilesWithObjectsInShard(order[k].second, mine);
				});
			}
			pool.join();
		}
		// Merge: (zoom, range of summary words) parts, each merging all threads' bitmaps
		std::vector<std::tuple<size_t, size_t, size_t>> parts;
		for (size_t zoom = 0; zoom < zooms.size(); zoom++) {
			const size_t words = precise[zoom]->summaryWords();
			const size_t step = std::max<size_t>(1, words / (threadNum * 4));
			for (size_t from = 0; from < words; from += step) parts.emplace_back(zoom, from, std::min(words, from + step));
		}
		std::atomic<size_t> nextPart(0);
		{
			boost::asio::thread_pool pool(threadNum);
			for (size_t t = 0; t < threadNum; t++) {
				boost::asio::post(pool, [&]() {
					for (size_t k; (k = nextPart++) < parts.size(); ) {
						const size_t zoom = std::get<0>(parts[k]);
						for (const auto& mine : local)
							precise[zoom]->mergeRange(static_cast<const PreciseTileCoordinatesSet&>(*mine[zoom]), std::get<1>(parts[k]), std::get<2>(parts[k]));
					}
				});
			}
			pool.join();
		}
		// The threads' bitmaps (32 MB each at z14) are freed on all threads too
		{
			boost::asio::thread_pool pool(threadNum);
			for (size_t t = 0; t < threadNum; t++)
				boost::asio::post(pool, [&, t]() { local[t].clear(); });
			pool.join();
		}
	}

	for (size_t i = 0; i < sources.size(); i++)
		sources[i]->collectTilesWithLargeObjectsAtZoom(zooms);
}

void sortOutputObjectIDs(
	const std::vector<bool>& sortOrders, 
	const AttributeStore& attributeStore,
	std::vector<OutputObjectID>& data
);

std::vector<OutputObjectID> TileDataSource::getObjectsForTile(
	const std::vector<bool>& sortOrders, 
	const AttributeStore& attributeStore,
	unsigned int zoom,
	TileCoordinates coordinates
) {
	std::vector<OutputObjectID> data;
	collectObjectsForTile(zoom, coordinates, data);
	collectLargeObjectsForTile(zoom, coordinates, data);
	sortOutputObjectIDs(sortOrders, attributeStore, data);
	data.erase(unique(data.begin(), data.end()), data.end());
	orderStoredGeometryTies(data);
	return data;
}

namespace {
struct GeometryTieKey {
	double x, y;      // first point
	uint64_t points;  // number of points
	uint64_t hash;    // of all points, only computed when the above are equal
	OutputObjectID o;
};

// Adds a ring/linestring to the key: first point and point count, or (withHash) the hash of all points
template<class R>
void addToTieKey(GeometryTieKey& key, const R& ring, bool withHash) {
	if (withHash) {
		key.hash = stableCombine(key.hash, static_cast<uint64_t>(ring.size()));
		for (const auto& p : ring) {
			key.hash = stableCombine(key.hash, static_cast<double>(p.x()));
			key.hash = stableCombine(key.hash, static_cast<double>(p.y()));
		}
		return;
	}
	if (key.points == 0 && !ring.empty()) {
		key.x = ring.front().x();
		key.y = ring.front().y();
	}
	key.points += ring.size();
}

void geometryTieKey(const TileDataSource& source, GeometryTieKey& key, bool withHash) {
	const NodeID objectID = key.o.oo.objectID;
	switch (key.o.oo.geomType) {
		case POINT_: {
			const Point& p = source.retrievePoint(objectID);
			addToTieKey(key, std::array<Point, 1>{p}, withHash);
			break;
		}
		case LINESTRING_:
			addToTieKey(key, source.retrieveLinestring(objectID), withHash);
			break;
		case MULTILINESTRING_:
			for (const auto& ls : source.retrieveMultiLinestring(objectID))
				addToTieKey(key, ls, withHash);
			break;
		case POLYGON_:
			for (const auto& poly : source.retrieveMultiPolygon(objectID)) {
				addToTieKey(key, poly.outer(), withHash);
				for (const auto& inner : poly.inners())
					addToTieKey(key, inner, withHash);
			}
			break;
	}
}
}

// Geometries stored at runtime (relations, shapefiles, ...) have an objectID that
// depends on which thread stored them, so the sort ties objects that share layer,
// z_order, type, attributes and id in an arbitrary order. Order such runs by
// geometry instead. OSM ways and nodes carry their OSM id in the objectID, and
// sort after stored geometries, so they never need this.
void TileDataSource::orderStoredGeometryTies(std::vector<OutputObjectID>& data) const {
	const NodeID storedLimit = 1ull << TILE_DATA_ID_SIZE;
	auto tied = [](const OutputObjectID& a, const OutputObjectID& b) {
		return a.oo.layer == b.oo.layer && a.oo.z_order == b.oo.z_order && a.oo.geomType == b.oo.geomType &&
		       a.oo.attributes == b.oo.attributes && a.id == b.id;
	};

	std::vector<GeometryTieKey> keys;
	size_t i = 0;
	while (i < data.size()) {
		size_t j = i + 1;
		if (data[i].oo.objectID < storedLimit)
			while (j < data.size() && data[j].oo.objectID < storedLimit && tied(data[i], data[j])) j++;
		if (j - i < 2) { i = j; continue; }

		keys.clear();
		for (size_t k = i; k < j; k++) {
			keys.push_back(GeometryTieKey{0, 0, 0, 0, data[k]});
			geometryTieKey(*this, keys.back(), false);
		}
		auto byPoint = [](const GeometryTieKey& a, const GeometryTieKey& b) {
			if (a.x != b.x) return a.x < b.x;
			if (a.y != b.y) return a.y < b.y;
			return a.points < b.points;
		};
		std::sort(keys.begin(), keys.end(), byPoint);

		// Still tied: hash all points (identical geometries may stay in any order)
		for (size_t a = 0; a < keys.size(); ) {
			size_t b = a + 1;
			while (b < keys.size() && !byPoint(keys[a], keys[b])) b++;
			if (b - a > 1) {
				for (size_t k = a; k < b; k++) geometryTieKey(*this, keys[k], true);
				std::sort(keys.begin() + a, keys.begin() + b, [](const GeometryTieKey& p, const GeometryTieKey& q) {
					if (p.hash != q.hash) return p.hash < q.hash;
					return p.o.oo.objectID < q.o.oo.objectID;
				});
			}
			a = b;
		}

		for (size_t k = i; k < j; k++) data[k] = keys[k - i].o;
		i = j;
	}
}

// ------------------------------------
// Add geometries to tile/large indices

namespace {
// Per-thread scratch for tile lists: clear() keeps the capacity, very large buffers are released.
thread_local std::vector<TileCoordinates> tlsIndexTiles, tlsIndexTilesTmp;
inline void releaseIfHuge(std::vector<TileCoordinates>& v) {
	if (v.capacity() > (1u << 16)) std::vector<TileCoordinates>().swap(v);
}

// A large multipolygon whose envelope holds more than this many times the
// tiles its outers cover is indexed by clusters of outers, not one envelope.
constexpr double SPARSE_ENVELOPE_RATIO = 4;
// Outers whose tile boxes are within this many tiles share a cluster.
constexpr double OUTER_CLUSTER_GAP = 4;

// The bounding box, in tile coordinates, of a non-empty list of tiles.
Box tileEnvelope(const std::vector<TileCoordinates>& tiles) {
	TileCoordinate minX = std::numeric_limits<TileCoordinate>::max(), maxX = 0;
	TileCoordinate minY = std::numeric_limits<TileCoordinate>::max(), maxY = 0;
	for (const TileCoordinates& index : tiles) {
		minX = std::min(index.x, minX);
		minY = std::min(index.y, minY);
		maxX = std::max(index.x, maxX);
		maxY = std::max(index.y, maxY);
	}
	return Box(geom::make<Point>(minX, minY), geom::make<Point>(maxX, maxY));
}

}

void TileDataSource::addGeometryToIndex(
	const Linestring& geom,
	const std::vector<OutputObject>& outputs,
	const uint64_t id
) {
	std::vector<TileCoordinates>& tiles = tlsIndexTiles;
	tiles.clear();
	try {
		insertIntermediateTiles(geom, indexZoom, tiles);

		bool polygonExists = false, otherExists = false;
		for (const auto& output : outputs) {
			if (output.geomType == POLYGON_) polygonExists = true;
			else otherExists = true;
		}
		if (tiles.empty()) polygonExists = false;
		TileCoordinate minTileX = std::numeric_limits<TileCoordinate>::max(), maxTileX = 0, minTileY = std::numeric_limits<TileCoordinate>::max(), maxTileY = 0;
		for (const TileCoordinates& index : tiles) {
			minTileX = std::min(index.x, minTileX);
			minTileY = std::min(index.y, minTileY);
			maxTileX = std::max(index.x, maxTileX);
			maxTileY = std::max(index.y, maxTileY);
		}
		if (otherExists)
			addToSmallIndex(tiles, id, [&](const TileCoordinates&, auto&& add) {
				for (const auto& output : outputs)
					if (output.geomType != POLYGON_) add(output); // not a polygon
			});

		// for polygon, fill inner tiles
		if (polygonExists) {
			bool tilesetFilled = false;
			uint size = (maxTileX - minTileX + 1) * (maxTileY - minTileY + 1);
			for (const auto& output : outputs) {
				if (output.geomType != POLYGON_) continue;
				if (size>= 16) {
					// Larger objects - add to rtree
					Box box = Box(geom::make<Point>(minTileX, minTileY),
					              geom::make<Point>(maxTileX, maxTileY));
					addObjectToLargeIndex(box, output, id);
				} else {
					// Smaller objects - add to each individual tile index
					if (!tilesetFilled) {
						fillCoveredTiles(tiles); tilesetFilled = true;
					}
					addToSmallIndex(tiles, id, [&](const TileCoordinates&, auto&& add) { add(output); });
				}
			}
		}
	} catch(std::out_of_range &err) {
		cerr << "Error calculating intermediate tiles: " << err.what() << endl;
	}
	releaseIfHuge(tiles);
}

void TileDataSource::addGeometryToIndex(
	const MultiLinestring& geom,
	const std::vector<OutputObject>& outputs,
	const uint64_t id
) {
	std::vector<TileCoordinates>& tiles = tlsIndexTiles;
	for (const auto& ls : geom) {
		tiles.clear();
		insertIntermediateTiles(ls, indexZoom, tiles);
		addToSmallIndex(tiles, id, [&](const TileCoordinates&, auto&& add) {
			for (const auto& output : outputs) add(output);
		});
	}
	releaseIfHuge(tiles);
}

void TileDataSource::addGeometryToIndex(
	const MultiPolygon& geom,
	std::vector<OutputObject>& outputs,
	const uint64_t id
) {
	// Each outer's tiles are sorted, unique and filled; concatenate them, then sort+unique if there are several outers
	std::vector<TileCoordinates>& tiles = tlsIndexTiles;
	std::vector<TileCoordinates>& tilesTmp = tlsIndexTilesTmp;
	std::vector<Box> outerBoxes;
	tiles.clear();
	for (const auto& poly : geom) {
		tilesTmp.clear();
		insertIntermediateTiles(poly.outer(), indexZoom, tilesTmp);
		fillCoveredTiles(tilesTmp);
		if (!tilesTmp.empty()) outerBoxes.push_back(tileEnvelope(tilesTmp));
		tiles.insert(tiles.end(), tilesTmp.begin(), tilesTmp.end());
	}
	if (geom.size() > 1) sortUniqueTiles(tiles);
	releaseIfHuge(tilesTmp);

	const size_t tileSetSize = tiles.size();
	std::vector<Box> boxes;
	if (tileSetSize >= 16) {
		// Larger objects go in the rtree, by bounding box. When the outers are
		// far apart, e.g. islands on both sides of the antimeridian, one box
		// around them all is mostly empty, and every tile in it would clip the
		// object to nothing; so each cluster of nearby outers gets its own box.
		const Box envelope = tileEnvelope(tiles);
		const double envelopeTiles =
			(envelope.max_corner().x() - envelope.min_corner().x() + 1) *
			(envelope.max_corner().y() - envelope.min_corner().y() + 1);
		if (outerBoxes.size() > 1 && envelopeTiles > SPARSE_ENVELOPE_RATIO * tileSetSize)
			boxes = merge_nearby_boxes(std::move(outerBoxes), OUTER_CLUSTER_GAP);
		else
			boxes.push_back(envelope);
	}

	if (tileSetSize >= 16) {
		for (auto& output : outputs) {
			for (const Box& box : boxes)
				addObjectToLargeIndex(box, output, id);
		}
	} else {
		// Smaller objects - add to each individual tile index
		addToSmallIndex(tiles, id, [&](const TileCoordinates&, auto&& add) {
			for (const auto& output : outputs) add(output);
		});
	}
	releaseIfHuge(tiles);
}

NodeID TileDataSource::storePoint(const Point& input) {
	const auto& store = pointStore.get(this);

	NodeID offset = store.second->size();
	store.second->push_back(input);
	NodeID rv = (store.first << (TILE_DATA_ID_SIZE - shardBits)) + offset;
	return rv;
}

NodeID TileDataSource::storeLinestring(const Linestring& src) {
	const auto& store = linestringStore.get(this);
	linestring_t dst(src.begin(), src.end());

	NodeID offset = store.second->size();
	store.second->emplace_back(std::move(dst));
	NodeID rv = (store.first << (TILE_DATA_ID_SIZE - shardBits)) + offset;
	return rv;
}

NodeID TileDataSource::storeMultiPolygon(const MultiPolygon& src, bool knownValid) {
	const auto& store = multipolygonStore.get(this);

	multi_polygon_t dst;
	dst.resize(src.size());
	for(std::size_t i = 0; i < src.size(); ++i) {
		// (the same points: a range copy, one allocation per ring)
		dst[i].outer().assign(src[i].outer().begin(), src[i].outer().end());

		dst[i].inners().resize(src[i].inners().size());
		for(std::size_t j = 0; j < src[i].inners().size(); ++j)
			dst[i].inners()[j].assign(src[i].inners()[j].begin(), src[i].inners()[j].end());
	}

	NodeID offset = store.second->size();
	store.second->emplace_back(std::move(dst));
	// The store is leased to this thread, so its validity flags are too
	multipolygonValid[store.first].push_back(knownValid);
	NodeID rv = (store.first << (TILE_DATA_ID_SIZE - shardBits)) + offset;
	return rv;
}

void TileDataSource::checkValidityLater(NodeID objectID, MultiPolygon mp) {
	auto valid = std::async(std::launch::async, [mp = std::move(mp)]() { RingPointIndexScope pointIndex; return geom::is_valid(mp); });
	std::lock_guard<std::mutex> lock(deferredValidityMutex);
	deferredValidity.emplace_back(objectID, std::move(valid));
}

void TileDataSource::finishDeferredValidity() {
	for (auto& entry : deferredValidity) {
		const bool valid = entry.second.get();
		auto& flags = multipolygonValid[getShard(entry.first)];
		const auto offset = getId(entry.first);
		if (offset < flags.size()) flags[offset] = valid;
	}
	deferredValidity.clear();
}

NodeID TileDataSource::storeMultiLinestring(const MultiLinestring& src) {
	const auto& store = multilinestringStore.get(this);

	multi_linestring_t dst;
	dst.resize(src.size());
	for (std::size_t i=0; i<src.size(); ++i) {
		boost::geometry::assign(dst[i], src[i]);
	}

	NodeID offset = store.second->size();
	store.second->emplace_back(std::move(dst));
	NodeID rv = (store.first << (TILE_DATA_ID_SIZE - shardBits)) + offset;
	return rv;
}

void TileDataSource::populateMultiPolygon(MultiPolygon& dst, NodeID objectID) {
	const auto &input = retrieveMultiPolygon(objectID);
	dst.resize(input.size());
	for(std::size_t i = 0; i < input.size(); ++i) {
		dst[i].outer().resize(input[i].outer().size());
		boost::geometry::assign(dst[i].outer(), input[i].outer());

		dst[i].inners().resize(input[i].inners().size());
		for(std::size_t j = 0; j < input[i].inners().size(); ++j) {
			dst[i].inners()[j].resize(input[i].inners()[j].size());
			boost::geometry::assign(dst[i].inners()[j], input[i].inners()[j]);
		}
	}
}
