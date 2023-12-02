/*! \file */ 
#ifndef _OSM_STORE_H
#define _OSM_STORE_H

#include "geom.h"
#include "coordinates.h"
#include "mmap_allocator.h"

#include <utility>
#include <vector>
#include <mutex>
#include <unordered_set>
#include <boost/container/flat_map.hpp>

extern bool verbose;

class NodeStore;
class WayStore;

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
	std::map<WayID, std::vector<WayID>> relationsForWays;
	std::map<WayID, tag_map_t> relationTags;
	mutable std::mutex mutex;

public:
	void relation_contains_way(WayID relid, WayID wayid) {
		std::lock_guard<std::mutex> lock(mutex);
		relationsForWays[wayid].emplace_back(relid);
	}
	void store_relation_tags(WayID relid, const tag_map_t &tags) {
		std::lock_guard<std::mutex> lock(mutex);
		relationTags[relid] = tags;
	}
	bool way_in_any_relations(WayID wayid) {
		return relationsForWays.find(wayid) != relationsForWays.end();
	}
	std::vector<WayID> relations_for_way(WayID wayid) {
		return relationsForWays[wayid];
	}
	std::string get_relation_tag(WayID relid, const std::string &key) {
		auto it = relationTags.find(relid);
		if (it==relationTags.end()) return "";
		auto jt = it->second.find(key);
		if (jt==it->second.end()) return "";
		return jt->second;
	}
	void clear() {
		std::lock_guard<std::mutex> lock(mutex);
		relationsForWays.clear();
		relationTags.clear();
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

	OSMStore will be mainly used for geometry generation. Geometry generation logic is implemented in this class.
	These functions are used by osm_output, and can be used by OsmLuaProcessing to provide the geometry information to Lua.

	Internal data structures are encapsulated in NodeStore, WayStore and [unused] RelationStore classes.
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
	using point_store_t = std::deque<std::pair<NodeID, Point>>;

	using linestring_t = boost::geometry::model::linestring<Point, std::vector, mmap_allocator>;
	using linestring_store_t = std::deque<std::pair<NodeID, linestring_t>>;

	using multi_linestring_t = boost::geometry::model::multi_linestring<linestring_t, std::vector, mmap_allocator>;
	using multi_linestring_store_t = std::deque<std::pair<NodeID, multi_linestring_t>>;

	using polygon_t = boost::geometry::model::polygon<Point, true, true, std::vector, std::vector, mmap_allocator, mmap_allocator>;
	using multi_polygon_t = boost::geometry::model::multi_polygon<polygon_t, std::vector, mmap_allocator>;
	using multi_polygon_store_t = std::deque<std::pair<NodeID, multi_polygon_t>>;

	struct generated {
		std::mutex points_store_mutex;
		std::unique_ptr<point_store_t> points_store;
		
		std::mutex linestring_store_mutex;
		std::unique_ptr<linestring_store_t> linestring_store;
		
		std::mutex multi_polygon_store_mutex;
		std::unique_ptr<multi_polygon_store_t> multi_polygon_store;

		std::mutex multi_linestring_store_mutex;
		std::unique_ptr<multi_linestring_store_t> multi_linestring_store;
	};

	NodeStore& nodes;
	WayStore& ways;
protected:	
	bool use_compact_nodes = false;
	bool require_integrity = true;

	RelationStore relations; // unused
	UsedWays used_ways;
	RelationScanStore scanned_relations;

	generated osm_generated;
	generated shp_generated;

public:

	OSMStore(NodeStore& nodes, WayStore& ways): nodes(nodes), ways(ways)
	{ 
		reopen();
	}

	void reopen();

	void open(std::string const &osm_store_filename);

	void use_compact_store(bool use) { use_compact_nodes = use; }
	void enforce_integrity(bool ei) { require_integrity = ei; }
	bool integrity_enforced() { return require_integrity; }

	void shapes_sort(unsigned int threadNum = 1);
	void generated_sort(unsigned int threadNum = 1);

	void nodes_sort(unsigned int threadNum);

	void ways_sort(unsigned int threadNum);

	void relations_insert_front(std::vector<RelationStore::element_t> &new_relations) {
		relations.insert_front(new_relations);
	}
	void relations_sort(unsigned int threadNum);

	void mark_way_used(WayID i) { used_ways.insert(i); }
	bool way_is_used(WayID i) { return used_ways.at(i); }

	void ensureUsedWaysInited();

	using tag_map_t = boost::container::flat_map<std::string, std::string>;
	void relation_contains_way(WayID relid, WayID wayid) { scanned_relations.relation_contains_way(relid,wayid); }
	void store_relation_tags(WayID relid, const tag_map_t &tags) { scanned_relations.store_relation_tags(relid,tags); }
	bool way_in_any_relations(WayID wayid) { return scanned_relations.way_in_any_relations(wayid); }
	std::vector<WayID> relations_for_way(WayID wayid) { return scanned_relations.relations_for_way(wayid); }
	std::string get_relation_tag(WayID relid, const std::string &key) { return scanned_relations.get_relation_tag(relid, key); }

	generated &osm() { return osm_generated; }
	generated const &osm() const { return osm_generated; }
	generated &shp() { return shp_generated; }
	generated const &shp() const { return shp_generated; }

	using handle_t = void *;

	template<typename T>
	void store_point(generated &store, NodeID id, T const &input) {	
		std::lock_guard<std::mutex> lock(store.points_store_mutex);
		store.points_store->emplace_back(id, input);		   	
	}

	Point const &retrieve_point(generated const &store, NodeID id) const {
		auto iter = std::lower_bound(store.points_store->begin(), store.points_store->end(), id, [](auto const &e, auto id) { 
			return e.first < id; 
		});

		if(iter == store.points_store->end() || iter->first != id)
			throw std::out_of_range("Could not find generated node with id " + std::to_string(id));

		return iter->second;
	}
	
	template<typename Input>
	void store_linestring(generated &store, NodeID id, Input const &src)
	{
		linestring_t dst(src.begin(), src.end());

		std::lock_guard<std::mutex> lock(store.linestring_store_mutex);
		store.linestring_store->emplace_back(id, std::move(dst));
	}

	linestring_t const &retrieve_linestring(generated const &store, NodeID id) const {
		auto iter = std::lower_bound(store.linestring_store->begin(), store.linestring_store->end(), id, [](auto const &e, auto id) { 
			return e.first < id; 
		});

		if(iter == store.linestring_store->end() || iter->first != id)
			throw std::out_of_range("Could not find generated linestring with id " + std::to_string(id));

		return iter->second;
	}
	
	template<typename Input>
	void store_multi_linestring(generated &store, NodeID id, Input const &src)
	{
		multi_linestring_t dst;
		dst.resize(src.size());
		for (std::size_t i=0; i<src.size(); ++i) {
			boost::geometry::assign(dst[i], src[i]);
		}

		std::lock_guard<std::mutex> lock(store.multi_linestring_store_mutex);
		store.multi_linestring_store->emplace_back(id, std::move(dst));
	}

	multi_linestring_t const &retrieve_multi_linestring(generated const &store, NodeID id) const {
		auto iter = std::lower_bound(store.multi_linestring_store->begin(), store.multi_linestring_store->end(), id, [](auto const &e, auto id) { 
			return e.first < id; 
		});

		if(iter == store.multi_linestring_store->end() || iter->first != id)
			throw std::out_of_range("Could not find generated multi-linestring with id " + std::to_string(id));

		return iter->second;
	}

	template<typename Input>
	void store_multi_polygon(generated &store, NodeID id, Input const &src)
	{
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
		
		std::lock_guard<std::mutex> lock(store.multi_polygon_store_mutex);
		store.multi_polygon_store->emplace_back(id, std::move(dst));
	}

	multi_polygon_t const &retrieve_multi_polygon(generated const &store, NodeID id) const {
		auto iter = std::lower_bound(store.multi_polygon_store->begin(), store.multi_polygon_store->end(), id, [](auto const &e, auto id) { 
			return e.first < id; 
		});

		if(iter == store.multi_polygon_store->end() || iter->first != id)
			throw std::out_of_range("Could not find generated multi polygon with id " + std::to_string(id));

		return iter->second;
	}

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
