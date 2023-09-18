/*! \file */ 
#ifndef _OSM_STORE_H
#define _OSM_STORE_H

#include "geom.h"
#include "coordinates.h"

#include <utility>
#include <vector>
#include <mutex>
#include <unordered_set>
#include <boost/container/flat_map.hpp>

extern bool verbose;

class void_mmap_allocator
{
public:
    typedef std::size_t size_type;

    static void *allocate(size_type n, const void *hint = 0);
    static void deallocate(void *p, size_type n);
    static void destroy(void *p);
	static void shutdown();
};

template<typename T>
class mmap_allocator
{

public:
    typedef std::size_t size_type;
    typedef std::ptrdiff_t difference_type;
    typedef T *pointer;
    typedef const T *const_pointer;
    typedef const T &const_reference;
    typedef T value_type;
    
    template <class U>
    struct rebind
    {
        typedef mmap_allocator<U> other;
    };
    
    mmap_allocator() = default;
    
    template<typename OtherT>
    mmap_allocator(OtherT &)
    { }
    
    pointer allocate(size_type n, const void *hint = 0)
    {
		return reinterpret_cast<T *>(void_mmap_allocator::allocate(n * sizeof(T), hint));
    }

    void deallocate(pointer p, size_type n)
    {
		void_mmap_allocator::deallocate(p, n);
    }

    void construct(pointer p, const_reference val)
    {
        new((void *)p) T(val);        
    }

    void destroy(pointer p) { void_mmap_allocator::destroy(p); }
};

template<typename T1, typename T2>
static inline bool operator==(mmap_allocator<T1> &, mmap_allocator<T2> &) { return true; }
template<typename T1, typename T2>
static inline bool operator!=(mmap_allocator<T1> &, mmap_allocator<T2> &) { return false; }

//
// Internal data structures.
//
class NodeStore
{

public:
	using element_t = std::pair<NodeID, LatpLon>;
	using map_t = std::deque<element_t, mmap_allocator<element_t>>;

	void reopen()
	{
		std::lock_guard<std::mutex> lock(mutex);
		mLatpLons = std::make_unique<map_t>();
	}

	// @brief Lookup a latp/lon pair
	// @param i OSM ID of a node
	// @return Latp/lon pair
	// @exception NotFound
	LatpLon at(NodeID i) const {
		auto iter = std::lower_bound(mLatpLons->begin(), mLatpLons->end(), i, [](auto const &e, auto i) { 
			return e.first < i; 
		});

		if(iter == mLatpLons->end() || iter->first != i)
			throw std::out_of_range("Could not find node with id " + std::to_string(i));

		return iter->second;
	}

	// @brief Return the number of stored items
	size_t size() const { 
		std::lock_guard<std::mutex> lock(mutex);
		return mLatpLons->size(); 
	}

	// @brief Insert a latp/lon pair.
	// @param i OSM ID of a node
	// @param coord a latp/lon pair to be inserted
	// @invariant The OSM ID i must be larger than previously inserted OSM IDs of nodes
	//			  (though unnecessarily for current impl, future impl may impose that)
	void insert_back(NodeID i, LatpLon coord) {
		mLatpLons->push_back(std::make_pair(i, coord));
	}

	void insert_back(std::vector<element_t> const &element) {
		std::lock_guard<std::mutex> lock(mutex);
		auto i = mLatpLons->size();
		mLatpLons->resize(i + element.size());
		std::copy(element.begin(), element.end(), mLatpLons->begin() + i);
	}

	// @brief Make the store empty
	void clear() { 
		std::lock_guard<std::mutex> lock(mutex);
		mLatpLons->clear(); 
	}

	void sort(unsigned int threadNum);

private: 
	mutable std::mutex mutex;
	std::shared_ptr<map_t> mLatpLons;
};

class CompactNodeStore
{

public:
	using element_t = std::pair<NodeID, LatpLon>;
	using map_t = std::deque<LatpLon, mmap_allocator<LatpLon>>;

	void reopen()
	{
		std::lock_guard<std::mutex> lock(mutex);
		mLatpLons = std::make_unique<map_t>();
	}

	// @brief Lookup a latp/lon pair
	// @param i OSM ID of a node
	// @return Latp/lon pair
	// @exception NotFound
	LatpLon at(NodeID i) const {
		if(i >= mLatpLons->size())
			throw std::out_of_range("Could not find node with id " + std::to_string(i));
		return mLatpLons->at(i);
	}

	// @brief Return the number of stored items
	size_t size() const { 
		std::lock_guard<std::mutex> lock(mutex);
		return mLatpLons->size(); 
	}

	// @brief Insert a latp/lon pair.
	// @param i OSM ID of a node
	// @param coord a latp/lon pair to be inserted
	// @invariant The OSM ID i must be larger than previously inserted OSM IDs of nodes
	//			  (though unnecessarily for current impl, future impl may impose that)
	void insert_back(NodeID i, LatpLon coord) {
		if(i >= mLatpLons->size())
			mLatpLons->resize(i + 1);
		(*mLatpLons)[i] = coord;
	}

	void insert_back(std::vector<element_t> const &elements) {
		std::lock_guard<std::mutex> lock(mutex);
		for(auto const &i: elements) 
			insert_back(i.first, i.second);
	}

	// @brief Make the store empty
	void clear() { 
		std::lock_guard<std::mutex> lock(mutex);
		mLatpLons->clear(); 
	}

private: 
	mutable std::mutex mutex;
	std::shared_ptr<map_t> mLatpLons;
};

// list of ways used by relations
// by noting these in advance, we don't need to store all ways in the store
class UsedWays {

private:
	std::vector<bool> usedList;
	mutable std::mutex mutex;

public:
	bool inited = false;

	// Size the vector to a reasonable estimate, to avoid resizing on the fly
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

// way store
class WayStore {

public:
	using latplon_vector_t = std::vector<LatpLon, mmap_allocator<LatpLon>>;
	using element_t = std::pair<WayID, latplon_vector_t>;
	using map_t = std::deque<element_t, mmap_allocator<element_t>>;

	void reopen() {
		mLatpLonLists = std::make_unique<map_t>();
	}

	// @brief Lookup a node list
	// @param i OSM ID of a way
	// @return A node list
	// @exception NotFound
	latplon_vector_t const &at(WayID wayid) const {
		std::lock_guard<std::mutex> lock(mutex);
		
		auto iter = std::lower_bound(mLatpLonLists->begin(), mLatpLonLists->end(), wayid, [](auto const &e, auto wayid) { 
			return e.first < wayid; 
		});

		if(iter == mLatpLonLists->end() || iter->first != wayid)
			throw std::out_of_range("Could not find way with id " + std::to_string(wayid));

		return iter->second;
	}

	// @brief Insert a node list.
	// @param i OSM ID of a way
	// @param llVec a coordinate vector to be inserted
	// @invariant The OSM ID i must be larger than previously inserted OSM IDs of ways
	//			  (though unnecessarily for current impl, future impl may impose that)
	void insert_back(std::vector<element_t> &new_ways) {
		std::lock_guard<std::mutex> lock(mutex);
		auto i = mLatpLonLists->size();
		mLatpLonLists->resize(i + new_ways.size());
		std::copy(std::make_move_iterator(new_ways.begin()), std::make_move_iterator(new_ways.end()), mLatpLonLists->begin() + i); 
	}

	// @brief Make the store empty
	void clear() { 
		std::lock_guard<std::mutex> lock(mutex);
		mLatpLonLists->clear(); 
	}

	std::size_t size() const { 
		std::lock_guard<std::mutex> lock(mutex);
		return mLatpLonLists->size(); 
	}

	void sort(unsigned int threadNum);

private:	
	mutable std::mutex mutex;
	std::unique_ptr<map_t> mLatpLonLists;
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
protected:	
	NodeStore nodes;
	CompactNodeStore compact_nodes;
	bool use_compact_nodes = false;
	bool require_integrity = true;

	WayStore ways;
	RelationStore relations; // unused
	UsedWays used_ways;
	RelationScanStore scanned_relations;

	void reopen() {
		nodes.reopen();
		compact_nodes.reopen();
		ways.reopen();
		relations.reopen();
	}

public:

	OSMStore()
	{ 
		reopen();
	}

	void open(std::string const &osm_store_filename);

	void use_compact_store(bool use = true) { use_compact_nodes = use; }
	void enforce_integrity(bool ei  = true) { require_integrity = ei; }
	bool integrity_enforced() { return require_integrity; }

	void nodes_insert_back(NodeID i, LatpLon coord) {
		if(!use_compact_nodes)
			nodes.insert_back(i, coord);
		else
			compact_nodes.insert_back(i, coord);
	}
	void nodes_insert_back(std::vector<NodeStore::element_t> const &new_nodes) {
		if(!use_compact_nodes)
			nodes.insert_back(new_nodes);
		else
			compact_nodes.insert_back(new_nodes);
	}
	void nodes_sort(unsigned int threadNum);
	std::size_t nodes_size() {
		return use_compact_nodes ? compact_nodes.size() : nodes.size();
	}

	LatpLon nodes_at(NodeID i) const { 
		return use_compact_nodes ? compact_nodes.at(i) : nodes.at(i);
	}

	void ways_insert_back(std::vector<WayStore::element_t> &new_ways) {
		ways.insert_back(new_ways);
	}
	void ways_sort(unsigned int threadNum);

	void relations_insert_front(std::vector<RelationStore::element_t> &new_relations) {
		relations.insert_front(new_relations);
	}
	void relations_sort(unsigned int threadNum);

	void mark_way_used(WayID i) { used_ways.insert(i); }
	bool way_is_used(WayID i) { return used_ways.at(i); }
	void ensure_used_ways_inited() {
		if (!used_ways.inited) used_ways.reserve(use_compact_nodes, nodes_size());
	}
	
	using tag_map_t = boost::container::flat_map<std::string, std::string>;
	void relation_contains_way(WayID relid, WayID wayid) { scanned_relations.relation_contains_way(relid,wayid); }
	void store_relation_tags(WayID relid, const tag_map_t &tags) { scanned_relations.store_relation_tags(relid,tags); }
	bool way_in_any_relations(WayID wayid) { return scanned_relations.way_in_any_relations(wayid); }
	std::vector<WayID> relations_for_way(WayID wayid) { return scanned_relations.relations_for_way(wayid); }
	std::string get_relation_tag(WayID relid, const std::string &key) { return scanned_relations.get_relation_tag(relid, key); }

	void clear() {
		nodes.clear();
		compact_nodes.clear();
		ways.clear();
		relations.clear();
		used_ways.clear();
	} 

	void reportStoreSize(std::ostringstream &str);
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
