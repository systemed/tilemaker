
#include "osm_store.h"
#include <iostream>
#include <fstream>
#include <iterator>
#include <unordered_map>

#include <ciso646>
#include "node_store.h"
#include "way_store.h"

using namespace std;
namespace bg = boost::geometry;

static inline bool isClosed(const std::vector<LatpLon>& way) {
	return way.begin() == way.end();
}

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
	ids.clear();
}

void OSMStore::open(std::string const &osm_store_filename)
{
	void_mmap_allocator::openMmapFile(osm_store_filename);
	reopen();
}

MultiPolygon OSMStore::wayListMultiPolygon(WayVec::const_iterator outerBegin, WayVec::const_iterator outerEnd, WayVec::const_iterator innerBegin, WayVec::const_iterator innerEnd) const {
	MultiPolygon mp;
	if (outerBegin == outerEnd) { return mp; } // no outers so quit

	std::vector<LatpLonDeque> outers;
	std::vector<LatpLonDeque> inners;
	std::map<WayID,bool> done; // true=this way has already been added to outers/inners, don't reconsider

	// merge constituent ways together
	mergeMultiPolygonWays(outers, done, outerBegin, outerEnd);
	mergeMultiPolygonWays(inners, done, innerBegin, innerEnd);

	// add all inners and outers to the multipolygon
	std::vector<Ring> filledInners;
	for (auto it = inners.begin(); it != inners.end(); ++it) {
		Ring inner;
		fillPoints(inner, it->begin(), it->end());
		filledInners.emplace_back(inner);
	}
	bool onlyOneOuter = outers.size()==1;
	for (auto ot = outers.begin(); ot != outers.end(); ot++) {
		Polygon poly;
		fillPoints(poly.outer(), ot->begin(), ot->end());
		for (auto it = filledInners.begin(); it != filledInners.end(); ++it) {
			if (onlyOneOuter || geom::within(*it, poly.outer())) { poly.inners().emplace_back(*it); }
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

	std::vector<LatpLonDeque> linestrings;
	std::map<WayID,bool> done;

	mergeMultiPolygonWays(linestrings, done, outerBegin, outerEnd);

	for (auto ot = linestrings.begin(); ot != linestrings.end(); ot++) {
		Linestring ls;
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
// There's quite a lot of copying going on here - could potentially be addressed
void OSMStore::mergeMultiPolygonWays(std::vector<LatpLonDeque> &results, std::map<WayID,bool> &done, WayVec::const_iterator itBegin, WayVec::const_iterator itEnd) const {

	// Create maps of start/end nodes
	std::unordered_map<LatpLon, std::vector<WayID>> startNodes;
	std::unordered_map<LatpLon, std::vector<WayID>> endNodes;
	for (auto it = itBegin; it != itEnd; ++it) {
		if (done[*it]) { continue; }
		try {
			auto const &way = ways.at(*it);
			if (isClosed(way) || results.empty()) {
				// if start==end, simply add it to the set
				results.emplace_back(way.begin(), way.end());
				done[*it] = true;
			} else {
				startNodes[way.front()].emplace_back(*it);
				endNodes[way.back()].emplace_back(*it);
			}
		} catch (std::out_of_range &err) {
			if (verbose) { cerr << "Missing way in relation: " << err.what() << endl; }
			done[*it] = true;
		}
	}

	auto deleteFromWayList = [&](LatpLon n, WayID w, bool which) {
		auto &nodemap = which ? startNodes : endNodes;
		std::vector<WayID> &waylist = nodemap.find(n)->second;
		waylist.erase(std::remove(waylist.begin(), waylist.end(), w), waylist.end());
		if (waylist.empty()) { nodemap.erase(nodemap.find(n)); }
	};
	auto removeWay = [&](WayID w) {
		auto const &way = ways.at(w);
		LatpLon first = way.front();
		LatpLon last  = way.back();
		if (startNodes.find(first) != startNodes.end()) { deleteFromWayList(first, w, true ); }
		if (startNodes.find(last)  != startNodes.end()) { deleteFromWayList(last,  w, true ); }
		if (endNodes.find(first)   != endNodes.end()  ) { deleteFromWayList(first, w, false); }
		if (endNodes.find(last)    != endNodes.end()  ) { deleteFromWayList(last,  w, false); }
		done[w] = true;
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
				if (startNodes.find(rLast)!=startNodes.end()) {
					// append to the result
					auto match = startNodes.find(rLast)->second;
					auto nodes = ways.at(match.back());
					rt->insert(rt->end(), nodes.begin(), nodes.end());
					removeWay(match.back());
					added++;

				} else if (endNodes.find(rLast)!=endNodes.end()) {
					// append reversed to the original
					auto match = endNodes.find(rLast)->second;
					auto nodes = ways.at(match.back());
					rt->insert(rt->end(),
						std::make_reverse_iterator(nodes.end()),
						std::make_reverse_iterator(nodes.begin()));
					removeWay(match.back());
					added++;

				} else if (endNodes.find(rFirst)!=endNodes.end()) {
					// prepend to the original
					auto match = endNodes.find(rFirst)->second;
					auto nodes = ways.at(match.back());
					rt->insert(rt->begin(), nodes.begin(), nodes.end());
					removeWay(match.back());
					added++;

				} else if (startNodes.find(rFirst)!=startNodes.end()) {
					// prepend reversed to the original
					auto match = startNodes.find(rFirst)->second;
					auto nodes = ways.at(match.back());
					rt->insert(rt->begin(),
						std::make_reverse_iterator(nodes.end()),
						std::make_reverse_iterator(nodes.begin()));
					removeWay(match.back());
					added++;

				} else { working=false; }
				
			} while (working);
		}

		// If nothing was added, then 'seed' it with a remaining unallocated way
		for (int i=0; i<=1; i++) {
			if (added>0) continue;
			for (auto nt : (i==0 ? startNodes : endNodes)) {
				WayID w = nt.second.back();
				auto const &way = ways.at(w);
				results.emplace_back(way.begin(), way.end());
				added++;
				removeWay(w);
				break;
			}
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


