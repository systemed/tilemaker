
#include "osm_store.h"
#include <iostream>
#include <fstream>
#include <iterator>
#include <unordered_map>
#include <boost/container/small_vector.hpp>

#include <ciso646>
#include <boost/geometry/index/rtree.hpp>
#include "node_store.h"
#include "way_store.h"

using namespace std;
namespace bg = boost::geometry;

static inline bool isClosed(const std::vector<LatpLon>& way) {
	return !way.empty() && way.front() == way.back();
}

// geom::within(inner, outer) for many inner rings against one big outer ring.
// Boost analyses the outer ring afresh on every call (a reservoir with 2500
// islands and a 600,000-point shore takes 47 s); here its segments are indexed
// once. An inner ring that shares no point with the outer ring lies wholly
// inside or wholly outside it, so one of its vertices decides; when the rings
// touch or cross, Boost decides as before.
class RingWithinIndex {
	using Segment = bg::model::segment<Point>;
	using Value = std::pair<Box, std::size_t>;
	const Ring &outer;
	bg::index::rtree<Value, bg::index::rstar<16>> segments;

	static std::vector<Value> segmentBoxes(const Ring &ring) {
		std::vector<Value> values;
		for (std::size_t i = 0; i + 1 < ring.size(); i++)
			values.emplace_back(bg::return_envelope<Box>(Segment(ring[i], ring[i + 1])), i);
		return values;
	}

public:
	RingWithinIndex(const Ring &outer) : outer(outer), segments(segmentBoxes(outer)) { }

	bool within(const Ring &inner) const {
		if (inner.size() < 4) return geom::within(inner, outer);
		for (std::size_t j = 0; j + 1 < inner.size(); j++) {
			const Segment s(inner[j], inner[j + 1]);
			for (auto it = segments.qbegin(bg::index::intersects(bg::return_envelope<Box>(s))); it != segments.qend(); ++it) {
				const std::size_t i = it->second;
				if (bg::intersects(s, Segment(outer[i], outer[i + 1])))
					return geom::within(inner, outer);
			}
		}
		return geom::within(inner.front(), outer);
	}
};

UsedObjects::UsedObjects(Status status): status(status), mutex(256), ids(256 * 1024) {
}

bool UsedObjects::test(NodeID id) {
	if (status == Status::Disabled)
		return true;

	const size_t chunk = id / 65536;
	if (ids[chunk].size() == 0)
		return false;

	return ids[chunk][id % 65536];
}

void UsedObjects::enable() {
	status = Status::Enabled;
	if (ids.empty())
		ids.resize(256 * 1024);
}

bool UsedObjects::enabled() const {
	return status == Status::Enabled;
}

void UsedObjects::set(NodeID id) {
	const size_t chunk = id / 65536;

	std::lock_guard<std::mutex> lock(mutex[chunk % mutex.size()]);
	if (ids[chunk].size() == 0)
		ids[chunk].resize(65536);

	ids[chunk][id % 65536] = true;
}

void UsedObjects::clear() {
	// This data is not needed after PbfProcessor's ReadPhase::Nodes has completed,
	// and it takes up to ~1.5GB of RAM.
	std::vector<std::vector<bool>>().swap(ids);
}

void OSMStore::open(std::string const &osm_store_filename)
{
	void_mmap_allocator::openMmapFile(osm_store_filename);
	reopen();
}

MultiPolygon OSMStore::wayListMultiPolygon(WayVec::const_iterator outerBegin, WayVec::const_iterator outerEnd, WayVec::const_iterator innerBegin, WayVec::const_iterator innerEnd) const {
	MultiPolygon mp;
	if (outerBegin == outerEnd) { return mp; } // no outers so quit

	std::vector<LatpLonChain> outers;
	std::vector<LatpLonChain> inners;
	DoneWays done; // the ways already added to outers/inners, not to reconsider

	// merge constituent ways together
	mergeMultiPolygonWays(outers, done, outerBegin, outerEnd);
	mergeMultiPolygonWays(inners, done, innerBegin, innerEnd);

	// add all inners and outers to the multipolygon
	std::vector<Ring> filledInners;
	std::vector<Box> innerBoxes;
	for (auto it = inners.begin(); it != inners.end(); ++it) {
		Ring inner;
		inner.reserve(it->size());
		fillPoints(inner, it->begin(), it->end());
		innerBoxes.emplace_back(geom::return_envelope<Box>(inner));
		filledInners.emplace_back(std::move(inner));
	}
	bool onlyOneOuter = outers.size()==1;
	for (auto ot = outers.begin(); ot != outers.end(); ot++) {
		Polygon poly;
		poly.outer().reserve(ot->size());
		fillPoints(poly.outer(), ot->begin(), ot->end());
		const Box outerBox = geom::return_envelope<Box>(poly.outer());
		// Index big outer rings that many inner rings must be tested against
		std::unique_ptr<RingWithinIndex> index;
		if (!onlyOneOuter && filledInners.size() >= 16 && poly.outer().size() >= 4096)
			index.reset(new RingWithinIndex(poly.outer()));
		{
			// The point-in-ring tests of the within tests below use ring_point_index's index of a
			// big ring (the same answers), while the rings don't change
			RingPointIndexScope pointIndex;
			for (std::size_t i = 0; i < filledInners.size(); i++) {
				const Ring &inner = filledInners[i];
				// A ring within the outer has its bounding box within the outer's,
				// so the (costly) within test is only needed when it does
				if (onlyOneOuter) {
					// (in no other polygon)
					poly.inners().emplace_back(std::move(filledInners[i]));
				} else if ((inner.empty() || geom::covered_by(innerBoxes[i], outerBox)) &&
				     (index ? index->within(inner) : geom::within(inner, poly.outer()))) {
					poly.inners().emplace_back(inner);
				}
			}
		}
		mp.emplace_back(move(poly));
	}

	// fix winding
	geom::correct(mp);
	return mp;
}

MultiLinestring OSMStore::wayListMultiLinestring(WayVec::const_iterator outerBegin, WayVec::const_iterator outerEnd) const {
	MultiLinestring mls;
	if (outerBegin == outerEnd) { return mls; }

	std::vector<LatpLonChain> linestrings;
	DoneWays done;

	mergeMultiPolygonWays(linestrings, done, outerBegin, outerEnd);

	for (auto ot = linestrings.begin(); ot != linestrings.end(); ot++) {
		Linestring ls;
		ls.reserve(ot->size());
		fillPoints(ls, ot->begin(), ot->end());
		mls.emplace_back(move(ls));
	}

	return mls;
}

// Assemble multipolygon constituent ways
// - Any closed polygons are added as-is
// - Linestrings are joined to existing linestrings with which they share a start/end
// - If no matches can be found, then one linestring is added (to 'attract' others)
// - The process is rerun until no ways are left
// (The ways are read once; the joined rings and lines are grown in place, see LatpLonChain.)
void OSMStore::mergeMultiPolygonWays(std::vector<LatpLonChain> &results, DoneWays &done, WayVec::const_iterator itBegin, WayVec::const_iterator itEnd) const {

	// Create maps of start/end nodes (the way lists are short: kept in the map's nodes)
	using WayList = boost::container::small_vector<WayID, 2>;
	using NodeMap = std::unordered_map<LatpLon, WayList>;
	NodeMap startNodes;
	NodeMap endNodes;
	// The coordinates of the ways in startNodes/endNodes, read once from the way store
	// (the joining below used to read each of them two or three more times), one after the other in pool
	std::vector<LatpLon> pool;
	std::unordered_map<WayID, std::pair<size_t, size_t>> wayCoords;	// offset in pool, size
	struct Span {
		const LatpLon *b, *e;
		const LatpLon *begin() const { return b; }
		const LatpLon *end() const { return e; }
		std::reverse_iterator<const LatpLon*> rbegin() const { return std::reverse_iterator<const LatpLon*>(e); }
		std::reverse_iterator<const LatpLon*> rend() const { return std::reverse_iterator<const LatpLon*>(b); }
		const LatpLon &front() const { return *b; }
		const LatpLon &back() const { return e[-1]; }
	};
	std::vector<LatpLon> way;
	auto coords = [&](WayID w) -> Span {
		auto it = wayCoords.find(w);
		if (it == wayCoords.end()) {
			ways.at(w, way);
			it = wayCoords.emplace(w, std::make_pair(pool.size(), way.size())).first;
			pool.insert(pool.end(), way.begin(), way.end());
		}
		return Span{ pool.data() + it->second.first, pool.data() + it->second.first + it->second.second };
	};
	for (auto it = itBegin; it != itEnd; ++it) {
		if (done.has(*it)) { continue; }
		try {
			ways.at(*it, way);
			if (isClosed(way) || results.empty()) {
				// if start==end, simply add it to the set
				results.emplace_back(std::move(way));
				way = std::vector<LatpLon>();
				done.add(*it);
			} else {
				startNodes[way.front()].emplace_back(*it);
				endNodes[way.back()].emplace_back(*it);
				wayCoords[*it] = std::make_pair(pool.size(), way.size());
				pool.insert(pool.end(), way.begin(), way.end());
			}
		} catch (std::out_of_range &err) {
			if (verbose) { cerr << "Missing way in relation: " << err.what() << endl; }
			done.add(*it);
		}
	}
	if (startNodes.empty()) return;	// (no way to join: all were added)

	auto deleteFromWayList = [&](NodeMap::iterator found, WayID w, NodeMap &nodemap) {
		WayList &waylist = found->second;
		waylist.erase(std::remove(waylist.begin(), waylist.end(), w), waylist.end());
		if (waylist.empty()) { nodemap.erase(found); }
	};
	auto removeWay = [&](WayID w) {
		const Span way = coords(w);
		LatpLon first = way.front();
		LatpLon last  = way.back();
		auto f = startNodes.find(first); if (f != startNodes.end()) { deleteFromWayList(f, w, startNodes); }
		f = startNodes.find(last);       if (f != startNodes.end()) { deleteFromWayList(f, w, startNodes); }
		f = endNodes.find(first);        if (f != endNodes.end()  ) { deleteFromWayList(f, w, endNodes); }
		f = endNodes.find(last);         if (f != endNodes.end()  ) { deleteFromWayList(f, w, endNodes); }
		done.add(w);
	};

	// Loop through, repeatedly adding start/end nodes if we can
	int added;
	do {
		added = 0;
		for (auto rt = results.begin(); rt != results.end(); rt++) {
			bool working=true;
			do {
				LatpLon rFirst = rt->front();
				LatpLon rLast  = rt->back();
				if (rFirst==rLast) break;
				auto match = startNodes.find(rLast);
				if (match!=startNodes.end()) {
					// append to the result
					const WayID w = match->second.back();
					const Span nodes = coords(w);
					rt->append(nodes.begin(), nodes.end());
					removeWay(w);
					added++;
					continue;
				}
				match = endNodes.find(rLast);
				if (match!=endNodes.end()) {
					// append reversed to the original
					const WayID w = match->second.back();
					const Span nodes = coords(w);
					rt->append(nodes.rbegin(), nodes.rend());
					removeWay(w);
					added++;
					continue;
				}
				match = endNodes.find(rFirst);
				if (match!=endNodes.end()) {
					// prepend to the original
					const WayID w = match->second.back();
					const Span nodes = coords(w);
					rt->prepend(nodes.begin(), nodes.end());
					removeWay(w);
					added++;
					continue;
				}
				match = startNodes.find(rFirst);
				if (match!=startNodes.end()) {
					// prepend reversed to the original
					const WayID w = match->second.back();
					const Span nodes = coords(w);
					rt->prepend(nodes.rbegin(), nodes.rend());
					removeWay(w);
					added++;
					continue;
				}
				working=false;
			} while (working);
		}

		// If nothing was added, then 'seed' it with a remaining unallocated way
		for (int i=0; i<=1; i++) {
			if (added>0) continue;
			auto &nodemap = i==0 ? startNodes : endNodes;
			if (nodemap.empty()) continue;
			WayID w = nodemap.begin()->second.back();
			const Span way = coords(w);
			results.emplace_back(way.begin(), way.end());
			added++;
			removeWay(w);
		}
	} while (added>0);
};


void OSMStore::reportSize() const {
	std::cout << "Stored " << nodes.size() << " nodes, " << ways.size() << " ways, " << relations.size() << " relations" << std::endl;
}

void OSMStore::reopen() {
	nodes.reopen();
	ways.reopen();
	relations.reopen();
}

void OSMStore::ensureUsedWaysInited() {
	if (!used_ways.inited) used_ways.reserve(use_compact_nodes, nodes.size());
}

void OSMStore::clear() {
	nodes.clear();
	ways.clear();
	relations.clear();
	used_ways.clear();
} 


