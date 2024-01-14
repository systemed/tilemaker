/*! \file */ 
#ifndef _OSM_STORE_H
#define _OSM_STORE_H

#include "geom.h"
#include "coordinates.h"
#include "mmap_allocator.h"
#include "relation_roles.h"

#include <utility>
#include <vector>
#include <mutex>
#include <unordered_set>
#include <boost/container/flat_map.hpp>
#include <protozero/data_view.hpp>

extern bool verbose;

class NodeStore;
class WayStore;

class UsedObjects {
public:
	enum class Status: bool { Disabled = false, Enabled = true };
	UsedObjects(Status status);
	bool test(NodeID id);
	void set(NodeID id);
	void enable();
	void clear();

private:
	Status status;
	std::vector<std::mutex> mutex;
	std::vector<std::vector<bool>> ids;
};

// A comparator for data_view so it can be used in boost's flat_map
struct DataViewLessThan {
	bool operator()(const protozero::data_view& a, const protozero::data_view& b) const {
		return a < b;
	}
};


//
// Internal data structures.
//
// list of ways used by relations
// by noting these in advance, we don't need to store all ways in the store
class UsedWays {

private:
	std::vector<bool> usedList;
	mutable std::mutex mutex;

public:
	bool inited = false;

	// Size the vector to a reasonable estimate, to avoid resizing on the fly
	// TODO: it'd be preferable if UsedWays didn't know about compact mode --
	//   instead, use an efficient data structure if numNodes < 1B, otherwise
	//   use a large bitvector
	void reserve(bool compact, size_t numNodes) {
		std::lock_guard<std::mutex> lock(mutex);
		if (inited) return;
		inited = true;
		if (compact) {
			// If we're running in compact mode, way count is roughly 1/9th of node count... say 1/8 to be safe
			usedList.reserve(numNodes/8);
		} else {
			// Otherwise, we could have anything up to the current max node ID (approaching 2**30 in summer 2021)
			// 2**31 is 0.25GB with a vector<bool>
			usedList.reserve(pow(2,31));
		}
	}
	
	// Mark a way as used
	void insert(WayID wayid) {
		std::lock_guard<std::mutex> lock(mutex);
		if (wayid>usedList.size()) usedList.resize(wayid+256);
		usedList[wayid] = true;
	}
	
	// See if a way is used
	bool at(WayID wayid) const {
		return (wayid>usedList.size()) ? false : usedList[wayid];
	}
	
	void clear() {
		std::lock_guard<std::mutex> lock(mutex);
		usedList.clear();
	}
};

// scanned relations store
class RelationScanStore {

private:
	using tag_map_t = boost::container::flat_map<std::string, std::string>;
	std::vector<std::map<WayID, std::vector<std::pair<RelationID, uint16_t>>>> relationsForWays;
	std::vector<std::map<NodeID, std::vector<std::pair<RelationID, uint16_t>>>> relationsForNodes;
	std::vector<std::map<RelationID, tag_map_t>> relationTags;
	mutable std::vector<std::mutex> mutex;
	RelationRoles relationRoles;

public:
	std::map<RelationID, std::vector<std::pair<RelationID, uint16_t>>> relationsForRelations;

	RelationScanStore(): relationsForWays(128), relationsForNodes(128), relationTags(128), mutex(128) {}

	void relation_contains_way(RelationID relid, WayID wayid, std::string role) {
		uint16_t roleId = relationRoles.getOrAddRole(role);
		const size_t shard = wayid % mutex.size();
		std::lock_guard<std::mutex> lock(mutex[shard]);
		relationsForWays[shard][wayid].emplace_back(std::make_pair(relid, roleId));
	}
	void relation_contains_node(RelationID relid, NodeID nodeId, std::string role) {
		uint16_t roleId = relationRoles.getOrAddRole(role);
		const size_t shard = nodeId % mutex.size();
		std::lock_guard<std::mutex> lock(mutex[shard]);
		relationsForNodes[shard][nodeId].emplace_back(std::make_pair(relid, roleId));
	}
	void relation_contains_relation(RelationID relid, RelationID relationId, std::string role) {
		uint16_t roleId = relationRoles.getOrAddRole(role);
		std::lock_guard<std::mutex> lock(mutex[0]);
		relationsForRelations[relationId].emplace_back(std::make_pair(relid, roleId));
	}
	void store_relation_tags(RelationID relid, const tag_map_t &tags) {
		const size_t shard = relid % mutex.size();
		std::lock_guard<std::mutex> lock(mutex[shard]);
		relationTags[shard][relid] = tags;
	}
	void set_relation_tag(RelationID relid, const std::string &key, const std::string &value) {
		const size_t shard = relid % mutex.size();
		std::lock_guard<std::mutex> lock(mutex[shard]);
		relationTags[shard][relid][key] = value;
	}
	bool way_in_any_relations(WayID wayid) {
		const size_t shard = wayid % mutex.size();
		return relationsForWays[shard].find(wayid) != relationsForWays[shard].end();
	}
	bool node_in_any_relations(NodeID nodeId) {
		const size_t shard = nodeId % mutex.size();
		return relationsForNodes[shard].find(nodeId) != relationsForNodes[shard].end();
	}
	bool relation_in_any_relations(RelationID relId) {
		return relationsForRelations.find(relId) != relationsForRelations.end();
	}
	std::string getRole(uint16_t roleId) const { return relationRoles.getRole(roleId); }
	const std::vector<std::pair<WayID, uint16_t>>& relations_for_way(WayID wayid) {
		const size_t shard = wayid % mutex.size();
		return relationsForWays[shard][wayid];
	}
	const std::vector<std::pair<RelationID, uint16_t>>& relations_for_node(NodeID nodeId) {
		const size_t shard = nodeId % mutex.size();
		return relationsForNodes[shard][nodeId];
	}
	const std::vector<std::pair<RelationID, uint16_t>>& relations_for_relation(RelationID relId) {
		return relationsForRelations[relId];
	}
	bool has_relation_tags(RelationID relId) {
		const size_t shard = relId % mutex.size();
		return relationTags[shard].find(relId) != relationTags[shard].end();
	}

	const tag_map_t& relation_tags(RelationID relId) {
		const size_t shard = relId % mutex.size();
		return relationTags[shard][relId];
	}
	// return all the parent relations (and their parents &c.) for a given relation
	std::vector<std::pair<RelationID, uint16_t>> relations_for_relation_with_parents(RelationID relId) {
		std::vector<RelationID> relationsToDo;
		std::set<RelationID> relationsDone;
		std::vector<std::pair<WayID, uint16_t>> out;
		relationsToDo.emplace_back(relId);
		// check parents in turn, pushing onto the stack if necessary
		while (!relationsToDo.empty()) {
			RelationID rel = relationsToDo.back();
			relationsToDo.pop_back();
			// check it's not already been added
			if (relationsDone.find(rel) != relationsDone.end()) continue;
			relationsDone.insert(rel);
			// add all its parents
			for (auto rp : relationsForRelations[rel]) {
				out.emplace_back(rp);
				relationsToDo.emplace_back(rp.first);
			}
		}
		return out;
	}
	std::string get_relation_tag(RelationID relid, const std::string &key) {
		const size_t shard = relid % mutex.size();
		auto it = relationTags[shard].find(relid);
		if (it==relationTags[shard].end()) return "";
		auto jt = it->second.find(key);
		if (jt==it->second.end()) return "";
		return jt->second;
	}
};


// relation store
// (this isn't currently used as we don't need to store relations for later processing, but may be needed for nested relations)

class RelationStore {

public:	
	using wayid_vector_t = std::vector<WayID, mmap_allocator<NodeID>>;
	using relation_entry_t = std::pair<wayid_vector_t, wayid_vector_t>;

	using element_t = std::pair<WayID, relation_entry_t>;
	using map_t = std::deque<element_t, mmap_allocator<element_t>>;

	void reopen() {
		std::lock_guard<std::mutex> lock(mutex);
		mOutInLists = std::make_unique<map_t>();
	}

	// @brief Insert a list of relations
	void insert_front(std::vector<element_t> &new_relations) {
		std::lock_guard<std::mutex> lock(mutex);
		auto i = mOutInLists->size();
		mOutInLists->resize(i + new_relations.size());
		std::copy(std::make_move_iterator(new_relations.begin()), std::make_move_iterator(new_relations.end()), mOutInLists->begin() + i); 
	}

	// @brief Make the store empty
	void clear() {
		std::lock_guard<std::mutex> lock(mutex);
		mOutInLists->clear();
	}

	std::size_t size() const {
		std::lock_guard<std::mutex> lock(mutex);
		return mOutInLists->size(); 
	}

private: 	
	mutable std::mutex mutex;
	std::unique_ptr<map_t> mOutInLists;
};

/**
	\brief OSM store keeps nodes, ways and relations in memory for later access

	Store all of those to be output: latp/lon for nodes, node list for ways, and way list for relations.
	It will serve as the global data store. OSM data destined for output will be set here from OsmMemTiles.

	Internal data structures are encapsulated in NodeStore, WayStore and RelationStore classes.
	These store can be altered for efficient memory use without global code changes.
	Such data structures have to return const ForwardInputIterators (only *, ++ and == should be supported).

	Possible future improvements to save memory:
	- pack WayStore (e.g. zigzag PBF encoding and varint)
	- combine innerWays and outerWays into one vector, with a single-byte index marking the changeover
	- use two arrays (sorted keys and elements) instead of map
*/
class OSMStore
{
public:
	NodeStore& nodes;
	WayStore& ways;
	RelationScanStore scannedRelations;

protected:	
	bool use_compact_nodes = false;
	bool require_integrity = true;

	RelationStore relations; // unused
	UsedWays used_ways;

public:
	UsedObjects usedNodes;
	UsedObjects usedRelations;

	OSMStore(NodeStore& nodes, WayStore& ways):
		nodes(nodes),
		ways(ways),
		// We only track usedNodes if way_keys is present; a node is used if it's
		// a member of a way used by a used relation, or a way that meets the way_keys
		// criteria.
		usedNodes(UsedObjects::Status::Disabled),
		// A relation is used only if it was previously accepted from relation_scan_function
		usedRelations(UsedObjects::Status::Enabled)
	{ 
		reopen();
	}

	void reopen();

	void open(std::string const &osm_store_filename);

	void use_compact_store(bool use) { use_compact_nodes = use; }
	void enforce_integrity(bool ei) { require_integrity = ei; }
	bool integrity_enforced() { return require_integrity; }

	void relations_insert_front(std::vector<RelationStore::element_t> &new_relations) {
		relations.insert_front(new_relations);
	}
	void relations_sort(unsigned int threadNum);

	void mark_way_used(WayID i) { used_ways.insert(i); }
	bool way_is_used(WayID i) { return used_ways.at(i); }

	void ensureUsedWaysInited();

	using tag_map_t = boost::container::flat_map<std::string, std::string>;

	void clear();
	void reportSize() const;

	// Relation -> MultiPolygon or MultiLinestring
	MultiPolygon wayListMultiPolygon(WayVec::const_iterator outerBegin, WayVec::const_iterator outerEnd, WayVec::const_iterator innerBegin, WayVec::const_iterator innerEnd) const;
	MultiLinestring wayListMultiLinestring(WayVec::const_iterator outerBegin, WayVec::const_iterator outerEnd) const;
	void mergeMultiPolygonWays(std::vector<LatpLonDeque> &results, std::map<WayID,bool> &done, WayVec::const_iterator itBegin, WayVec::const_iterator itEnd) const;

	///It is not really meaningful to try using a relation as a linestring. Not normally used but included
	///if Lua script attempts to do this.
	//
	// Relation -> MultiPolygon
	static Linestring wayListLinestring(MultiPolygon const &mp) {
		Linestring out;
		if(!mp.empty()) {
			for(auto pt: mp[0].outer())
				boost::geometry::append(out, pt);
		}
		return out;
	}

	template<class WayIt>
	Polygon llListPolygon(WayIt begin, WayIt end) const {
		Polygon poly;
		fillPoints(poly.outer(), begin, end);
		boost::geometry::correct(poly);
		return poly;
	}

	// Way -> Linestring
	template<class WayIt>
	Linestring llListLinestring(WayIt begin, WayIt end) const {
		Linestring ls;
		fillPoints(ls, begin, end);
		return ls;
	}

private:
	// helper
	template<class PointRange, class LatpLonIt>
	void fillPoints(PointRange &points, LatpLonIt begin, LatpLonIt end) const {
		for (auto it = begin; it != end; ++it) {
			try {
				boost::geometry::range::push_back(points, boost::geometry::make<Point>(it->lon/10000000.0, it->latp/10000000.0));
			} catch (std::out_of_range &err) {
				if (require_integrity) throw err;
			}
		}
	}
};

#endif //_OSM_STORE_H
