/*! \file */ 
#ifndef _OSM_STORE_H
#define _OSM_STORE_H

#include "geom.h"
#include "coordinates.h"
#include "mmap_allocator.h"
#include "relation_roles.h"

#include <algorithm>
#include <atomic>
#include <utility>
#include <vector>
#include <mutex>
#include <thread>
#include <memory>
#include <unordered_set>
#include <boost/container/flat_map.hpp>
#ifdef _MSC_VER
#include <intrin.h>
#endif
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
	bool enabled() const;
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
	// Member ways of accepted relations. While the relations are scanned, each thread collects its
	// own list (insertMany, no lock); finalize() turns them into the set: a bitmap if it is dense
	// relative to the ID range (planet-scale inputs), else a sorted list.
	struct Collected { std::vector<WayID> ids; };
	std::mutex registryMutex;
	std::vector<std::unique_ptr<Collected>> collected;	// one per thread that inserted
	std::vector<WayID> ids;	// sparse set (sorted, unique)
	std::vector<uint64_t> dense;	// dense set: bit i of word i/64
	bool isDense = false;
	std::atomic<bool> sorted{true};
	mutable std::mutex mutex;
	const uint64_t instance = nextInstance();	// never reused, unlike the address

	static uint64_t nextInstance() {
		static std::atomic<uint64_t> next(0);
		return next++;
	}

	std::vector<WayID> &mine() {
		thread_local std::vector<std::pair<uint64_t, std::vector<WayID> *>> lists;
		for (auto &e : lists) if (e.first == instance) return *e.second;
		std::lock_guard<std::mutex> lock(registryMutex);
		collected.emplace_back(new Collected());
		lists.emplace_back(instance, &collected.back()->ids);
		return collected.back()->ids;
	}

	// All collected lists (and the sparse list) into the set; with mutex held
	void finalizeLocked() {
		std::vector<std::vector<WayID> *> lists;
		{
			std::lock_guard<std::mutex> lock(registryMutex);
			for (auto &c : collected) if (!c->ids.empty()) lists.push_back(&c->ids);
		}
		size_t total = ids.size();
		WayID maxId = ids.empty() ? 0 : ids.back();
		for (auto *l : lists) {
			total += l->size();
			for (WayID id : *l) maxId = std::max(maxId, id);
		}
		if (!isDense && total > 0 && total * 64 > maxId) {
			// dense: set the bits on all threads (atomic OR, as lists overlap)
			dense.assign(maxId / 64 + 1, 0);
			isDense = true;
			for (WayID id : ids) dense[id / 64] |= uint64_t(1) << (id % 64);
			std::vector<WayID>().swap(ids);
		}
		if (isDense) {
			for (auto *l : lists) for (WayID id : *l) if (id / 64 >= dense.size()) dense.resize(id / 64 + 1, 0);
			std::atomic<size_t> next(0);
			const unsigned workers = std::max(1u, std::min<unsigned>(std::thread::hardware_concurrency(), lists.size()));
			std::vector<std::thread> threads;
			for (unsigned t = 0; t < workers; t++)
				threads.emplace_back([&]() {
					for (size_t k = next++; k < lists.size(); k = next++)
						for (WayID id : *lists[k])
#ifdef _MSC_VER
							_InterlockedOr64(reinterpret_cast<volatile __int64 *>(&dense[id / 64]), static_cast<__int64>(uint64_t(1) << (id % 64)));
#else
							__atomic_fetch_or(&dense[id / 64], uint64_t(1) << (id % 64), __ATOMIC_RELAXED);
#endif
				});
			for (auto &t : threads) t.join();
		} else {
			for (auto *l : lists) ids.insert(ids.end(), l->begin(), l->end());
			std::sort(ids.begin(), ids.end());
			ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
			ids.shrink_to_fit();
		}
		for (auto *l : lists) std::vector<WayID>().swap(*l);
		sorted.store(true, std::memory_order_release);
	}

public:
	bool inited = false;

	void reserve(bool compact, size_t numNodes) {
		std::lock_guard<std::mutex> lock(mutex);
		if (inited) return;
		inited = true;
	}

	// Mark ways as used (collected per thread until finalize)
	void insertMany(const std::vector<WayID> &wayids) {
		if (wayids.empty()) return;
		std::vector<WayID> &list = mine();
		list.insert(list.end(), wayids.begin(), wayids.end());
		sorted.store(false, std::memory_order_relaxed);
	}

	// Mark a way as used
	void insert(WayID wayid) {
		mine().push_back(wayid);
		sorted.store(false, std::memory_order_relaxed);
	}

	// Build the set; call once all inserts are done, before any at()
	void finalize() {
		std::lock_guard<std::mutex> lock(mutex);
		finalizeLocked();
	}

	// See if a way is used
	bool at(WayID wayid) {
		if (!sorted.load(std::memory_order_acquire)) {
			std::lock_guard<std::mutex> lock(mutex);
			if (!sorted.load(std::memory_order_relaxed)) finalizeLocked();
		}
		if (isDense)
			return wayid / 64 < dense.size() && ((dense[wayid / 64] >> (wayid % 64)) & 1);
		return std::binary_search(ids.begin(), ids.end(), wayid);
	}

	void clear() {
		std::lock_guard<std::mutex> lock(mutex);
		{
			std::lock_guard<std::mutex> lock2(registryMutex);
			for (auto &c : collected) std::vector<WayID>().swap(c->ids);
		}
		ids.clear();
		ids.shrink_to_fit();
		dense.clear();
		dense.shrink_to_fit();
		isDense = false;
		sorted.store(true, std::memory_order_release);
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

	// Every way is looked up once while reading ways, mostly not found. relationsForWays is
	// sharded by way id % 128, and each shard (a map) is ordered by way id: wayIndex holds each
	// shard as a sorted array (filled during the relation scan, before; rebuilt on first use after
	// relationsForWays changed). Ways are read in id order, so a reader that keeps where it was
	// in each shard (WayCursor) finds the next way a few entries further on, in the same cache
	// lines, instead of a random access into one big table.
	using WayRelations = std::vector<std::pair<RelationID, uint16_t>>;
	struct WayIndexSlot { WayID id; WayRelations* relations; };
	std::vector<std::vector<WayIndexSlot>> wayIndex;
	std::atomic<bool> wayIndexStale { true };
	std::mutex wayIndexMutex;
	void ensureWayIndex() {
		if (wayIndexStale.load(std::memory_order_acquire)) {
			std::lock_guard<std::mutex> lock(wayIndexMutex);
			if (wayIndexStale.load(std::memory_order_relaxed)) {
				std::vector<std::vector<WayIndexSlot>> index(relationsForWays.size());
				for (size_t s = 0; s < relationsForWays.size(); s++) {
					index[s].reserve(relationsForWays[s].size());
					for (auto& entry : relationsForWays[s]) index[s].push_back({ entry.first, &entry.second });
				}
				wayIndex.swap(index);
				wayIndexStale.store(false, std::memory_order_release);
			}
		}
	}
	// The way's entry in shard `slots` (nullptr: none), searched from `pos` onwards when the way
	// isn't before it (galloping), else in [0, pos); `pos` is left at the way's position
	static WayRelations* findInShard(const std::vector<WayIndexSlot>& slots, WayID wayid, uint32_t& pos) {
		const size_t n = slots.size();
		size_t lo, hi;	// the way's position is in [lo, hi]
		if (pos < n && slots[pos].id <= wayid) {
			lo = pos; size_t step = 1;
			while (lo + step < n && slots[lo + step].id < wayid) { lo += step; step *= 2; }
			hi = std::min(lo + step, n);
		} else {
			lo = 0; hi = std::min<size_t>(pos, n);
		}
		const auto it = std::lower_bound(slots.begin() + lo, slots.begin() + hi, wayid,
			[](const WayIndexSlot& s, WayID id) { return s.id < id; });
		pos = it - slots.begin();
		return it != slots.end() && it->id == wayid ? it->relations : nullptr;
	}
	WayRelations* findWay(WayID wayid) {
		ensureWayIndex();
		uint32_t pos = 0;
		return findInShard(wayIndex[wayid % wayIndex.size()], wayid, pos);
	}

public:
	// A reader's positions in wayIndex, see findWayRelations
	struct WayCursor { std::vector<uint32_t> pos; };
	// The relations way `wayid` is in (nullptr: none), as relations_for_way; for a reader going
	// through ways in id order, with its own cursor
	const WayRelations* findWayRelations(WayID wayid, WayCursor& cursor) {
		ensureWayIndex();
		if (cursor.pos.size() != wayIndex.size()) cursor.pos.assign(wayIndex.size(), 0);
		const size_t shard = wayid % wayIndex.size();
		return findInShard(wayIndex[shard], wayid, cursor.pos[shard]);
	}

public:
	std::map<RelationID, std::vector<std::pair<RelationID, uint16_t>>> relationsForRelations;

	RelationScanStore(): relationsForWays(128), relationsForNodes(128), relationTags(128), mutex(128) {}

	// Frees what was scanned: only the Lua functions use it, while the .pbf files are read. With the
	// bundled profile (administrative boundaries) it took ~1.1 GB on a planet, kept to the end.
	void clear() {
		for (auto& shard : relationsForWays) shard.clear();
		for (auto& shard : relationsForNodes) shard.clear();
		for (auto& shard : relationTags) shard.clear();
		relationsForRelations.clear();
		std::vector<std::vector<WayIndexSlot>>().swap(wayIndex);
		wayIndexStale.store(true, std::memory_order_release);
	}

	// Keep each member's parent relations ordered by relation id: relations are scanned on
	// several threads, so appending would make the order NextRelation() returns depend on
	// thread scheduling. Entries of the same relation keep their member order.
	static void insertByRelation(std::vector<std::pair<RelationID, uint16_t>>& list, RelationID relid, uint16_t roleId) {
		const auto it = std::upper_bound(list.begin(), list.end(), relid,
			[](RelationID id, const std::pair<RelationID, uint16_t>& entry) { return id < entry.first; });
		list.emplace(it, relid, roleId);
	}

	void relation_contains_way(RelationID relid, WayID wayid, std::string role) {
		uint16_t roleId = relationRoles.getOrAddRole(role);
		const size_t shard = wayid % mutex.size();
		std::lock_guard<std::mutex> lock(mutex[shard]);
		insertByRelation(relationsForWays[shard][wayid], relid, roleId);
		wayIndexStale.store(true, std::memory_order_release);
	}
	void relation_contains_node(RelationID relid, NodeID nodeId, std::string role) {
		uint16_t roleId = relationRoles.getOrAddRole(role);
		const size_t shard = nodeId % mutex.size();
		std::lock_guard<std::mutex> lock(mutex[shard]);
		insertByRelation(relationsForNodes[shard][nodeId], relid, roleId);
	}
	void relation_contains_relation(RelationID relid, RelationID relationId, std::string role) {
		uint16_t roleId = relationRoles.getOrAddRole(role);
		std::lock_guard<std::mutex> lock(mutex[0]);
		insertByRelation(relationsForRelations[relationId], relid, roleId);
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
		return findWay(wayid) != nullptr;
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
		if (WayRelations* relations = findWay(wayid)) return *relations;
		// Not a member of any relation (callers check first): as before, an empty list
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
			auto it = relationsForRelations.find(rel);
			if (it == relationsForRelations.end()) continue;
			for (auto rp : it->second) {
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
	bool isCompactStore() { return use_compact_nodes; }
	void enforce_integrity(bool ei) { require_integrity = ei; }
	bool integrity_enforced() { return require_integrity; }

	void relations_insert_front(std::vector<RelationStore::element_t> &new_relations) {
		relations.insert_front(new_relations);
	}
	void relations_sort(unsigned int threadNum);

	void mark_way_used(WayID i) { used_ways.insert(i); }
	void mark_ways_used(const std::vector<WayID> &ids) { used_ways.insertMany(ids); }
	bool way_is_used(WayID i) { return used_ways.at(i); }

	void ensureUsedWaysInited();
	void finalizeUsedWays() { used_ways.finalize(); }

	using tag_map_t = boost::container::flat_map<std::string, std::string>;

	void clear();
	// Once all .pbf files are read: frees the relation scan and the set of used ways, which only
	// reading needs (the node, way and relation stores stay, the tiles are built from them)
	void releaseReadingState();
	void reportSize() const;

	// Relation -> MultiPolygon or MultiLinestring
	MultiPolygon wayListMultiPolygon(WayVec::const_iterator outerBegin, WayVec::const_iterator outerEnd, WayVec::const_iterator innerBegin, WayVec::const_iterator innerEnd) const;
	MultiLinestring wayListMultiLinestring(WayVec::const_iterator outerBegin, WayVec::const_iterator outerEnd) const;
	// A ring or line being joined from ways: points added at either end, in one buffer with room
	// in front (instead of a std::deque)
	struct LatpLonChain {
		std::vector<LatpLon> buf;
		size_t head = 0;	// the points are buf[head...]
		LatpLonChain() = default;
		template<class It> LatpLonChain(It b, It e) : buf(b, e) { }
		explicit LatpLonChain(std::vector<LatpLon> &&v) : buf(std::move(v)) { }
		const LatpLon &front() const { return buf[head]; }
		const LatpLon &back() const { return buf.back(); }
		std::vector<LatpLon>::const_iterator begin() const { return buf.cbegin() + head; }
		std::vector<LatpLon>::const_iterator end() const { return buf.cend(); }
		size_t size() const { return buf.size() - head; }
		template<class It> void append(It b, It e) { buf.insert(buf.end(), b, e); }
		template<class It> void prepend(It b, It e) {
			const size_t n = std::distance(b, e);
			if (n > head) {
				// move the points back, leaving room for these and as many again as there are
				const size_t count = size(), room = n + count;
				std::vector<LatpLon> grown(room + count);
				std::copy(begin(), end(), grown.begin() + room);
				buf.swap(grown);
				head = room;
			}
			head -= n;
			std::copy(b, e, buf.begin() + head);
		}
	};
	// The ways merged (mergeMultiPolygonWays): a short list, a hash set once there are many
	class DoneWays {
		std::vector<WayID> few;
		std::unordered_set<WayID> many;
	public:
		bool has(WayID w) const {
			if (!many.empty()) return many.count(w) > 0;
			return std::find(few.begin(), few.end(), w) != few.end();
		}
		void add(WayID w) {
			if (!many.empty()) { many.insert(w); return; }
			if (std::find(few.begin(), few.end(), w) != few.end()) return;
			few.push_back(w);
			if (few.size() > 32) many.insert(few.begin(), few.end());
		}
	};
	void mergeMultiPolygonWays(std::vector<LatpLonChain> &results, DoneWays &done, WayVec::const_iterator itBegin, WayVec::const_iterator itEnd) const;

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
		poly.outer().reserve(end - begin);
		fillPoints(poly.outer(), begin, end);
		boost::geometry::correct(poly);
		return poly;
	}

	// Way -> Linestring
	template<class WayIt>
	Linestring llListLinestring(WayIt begin, WayIt end) const {
		Linestring ls;
		ls.reserve(end - begin);
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
