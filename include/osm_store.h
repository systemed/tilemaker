/*! \file */ 
#ifndef _OSM_STORE_H
#define _OSM_STORE_H

#include "geom.h"
#include "coordinates.h"

#include <utility>
#include <vector>
#include <mutex>

class void_mmap_allocator
{
public:
    typedef std::size_t size_type;

    static void *allocate(size_type n, const void *hint = 0);
    static void deallocate(void *p, size_type n);
    static void destroy(void *p);
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

// way store
class WayStore {

public:
	using nodeid_vector_t = std::vector<NodeID, mmap_allocator<NodeID>>;
	using element_t = std::pair<NodeID, nodeid_vector_t>;
	using map_t = std::deque<element_t, mmap_allocator<element_t>>;

	void reopen() {
		mNodeLists = std::make_unique<map_t>();
	}

	// @brief Lookup a node list
	// @param i OSM ID of a way
	// @return A node list
	// @exception NotFound
	nodeid_vector_t const &at(WayID wayid) const {
		std::lock_guard<std::mutex> lock(mutex);
		
		auto iter = std::lower_bound(mNodeLists->begin(), mNodeLists->end(), wayid, [](auto const &e, auto wayid) { 
			return e.first < wayid; 
		});

		if(iter == mNodeLists->end() || iter->first != wayid)
			throw std::out_of_range("Could not find way with id " + std::to_string(wayid));

		return iter->second;
	}

	// @brief Insert a node list.
	// @param i OSM ID of a way
	// @param nodeVec a node vector to be inserted
	// @invariant The OSM ID i must be larger than previously inserted OSM IDs of ways
	//			  (though unnecessarily for current impl, future impl may impose that)
	void insert_back(std::vector<element_t> &new_ways) {
		std::lock_guard<std::mutex> lock(mutex);
		auto i = mNodeLists->size();
		mNodeLists->resize(i + new_ways.size());
		std::copy(std::make_move_iterator(new_ways.begin()), std::make_move_iterator(new_ways.end()), mNodeLists->begin() + i); 
	}

	// @brief Make the store empty
	void clear() { 
		std::lock_guard<std::mutex> lock(mutex);
		mNodeLists->clear(); 
	}

	std::size_t size() const { 
		std::lock_guard<std::mutex> lock(mutex);
		return mNodeLists->size(); 
	}

	void sort(unsigned int threadNum);

private:	
	mutable std::mutex mutex;
	std::unique_ptr<map_t> mNodeLists;
};

// relation store

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
	using point_store_t = std::deque<Point>;

	using linestring_t = boost::geometry::model::linestring<Point, std::vector, mmap_allocator>;
	using linestring_store_t = std::deque<linestring_t>;

	using polygon_t = boost::geometry::model::polygon<Point, true, true, std::vector, std::vector, mmap_allocator, mmap_allocator>;
	using multi_polygon_t = boost::geometry::model::multi_polygon<polygon_t, std::vector, mmap_allocator>;
	using multi_polygon_store_t = std::deque<multi_polygon_t>;

	struct generated {
		std::mutex points_store_mutex;
		std::unique_ptr<point_store_t> points_store;
		
		std::mutex linestring_store_mutex;
		std::unique_ptr<linestring_store_t> linestring_store;
		
		std::mutex multi_polygon_store_mutex;
		std::unique_ptr<multi_polygon_store_t> multi_polygon_store;
	};

protected:	
	NodeStore nodes;
	WayStore ways;
	RelationStore relations;

	generated osm_generated;
	generated shp_generated;

	void reopen() {
		nodes.reopen();
		ways.reopen();
		relations.reopen();
		
		osm_generated.points_store = std::make_unique<point_store_t>();
		osm_generated.linestring_store = std::make_unique<linestring_store_t>();
		osm_generated.multi_polygon_store = std::make_unique<multi_polygon_store_t>();

		shp_generated.points_store = std::make_unique<point_store_t>();
		shp_generated.linestring_store = std::make_unique<linestring_store_t>();
		shp_generated.multi_polygon_store = std::make_unique<multi_polygon_store_t>();
	}

public:

	OSMStore()
	{ 
		reopen();
	}

	~OSMStore();

	void open(std::string const &osm_store_filename);

	void nodes_insert_back(NodeID i, LatpLon coord) {
		nodes.insert_back(i, coord);
	}
	void nodes_insert_back(std::vector<NodeStore::element_t> const &new_nodes) {
		nodes.insert_back(new_nodes);
	}
	void nodes_sort(unsigned int threadNum) {
		nodes.sort(threadNum);
	}

	void ways_insert_back(std::vector<WayStore::element_t> &new_ways) {
		ways.insert_back(new_ways);
	}
	void ways_sort(unsigned int threadNum) { 
		ways.sort(threadNum); 
	}

	void relations_insert_front(std::vector<RelationStore::element_t> &new_relations) {
		relations.insert_front(new_relations);
	}

	generated &osm() { return osm_generated; }
	generated const &osm() const { return osm_generated; }
	generated &shp() { return shp_generated; }
	generated const &shp() const { return shp_generated; }

	using handle_t = void *;

	template<typename T>
	static T const &retrieve(handle_t handle) 
	{
		return *reinterpret_cast<T const *>(handle);
	}

	template<typename T>
	handle_t store_point(generated &store, T const &input) {	
		std::lock_guard<std::mutex> lock(store.points_store_mutex);
		store.points_store->push_back(input);		   	
		return &store.points_store->back();		   	
	}

	Point retrieve_point(generated const &store, std::size_t id) const {
		return store.points_store->at(id);		   
	}
	
	template<typename Input>
	handle_t store_linestring(generated &store, Input const &src)
	{
		linestring_t dst(src.begin(), src.end());

		std::lock_guard<std::mutex> lock(store.linestring_store_mutex);
		store.linestring_store->emplace_back(std::move(dst));
		return &store.linestring_store->back();
	}

	linestring_t const &retrieve_linestring(generated const &store, std::size_t id) const {
		return store.linestring_store->at(id);
	}

	template<typename Input>
	handle_t store_multi_polygon(generated &store, Input const &src)
	{
		multi_polygon_t dst;
		boost::geometry::assign(dst, src);
		
		std::lock_guard<std::mutex> lock(store.multi_polygon_store_mutex);
		std::size_t id = store.multi_polygon_store->size();
		store.multi_polygon_store->emplace_back(std::move(dst));
		return &store.multi_polygon_store->back();
	}

	multi_polygon_t const &retrieve_multi_polygon(generated const &store, std::size_t id) const {
		return store.multi_polygon_store->at(id);
	}

	void clear() {
		nodes.clear();
		ways.clear();
		relations.clear();
	} 

	void reportStoreSize(std::ostringstream &str);
	void reportSize() const;

	// Relation -> MultiPolygon
	MultiPolygon wayListMultiPolygon(WayVec::const_iterator outerBegin, WayVec::const_iterator outerEnd, WayVec::const_iterator innerBegin, WayVec::const_iterator innerEnd) const;
	void mergeMultiPolygonWays(std::vector<NodeVec> &results, std::map<WayID,bool> &done, WayVec::const_iterator itBegin, WayVec::const_iterator itEnd) const;

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
	Polygon nodeListPolygon(WayIt begin, WayIt end) const {
		Polygon poly;
		fillPoints(poly.outer(), begin, end);
		boost::geometry::correct(poly);
		return poly;
	}

	// Way -> Linestring
	template<class WayIt>
	Linestring nodeListLinestring(WayIt begin, WayIt end) const {
		Linestring ls;
		fillPoints(ls, begin, end);
		return ls;
	}

private:
	// helper
	template<class PointRange, class NodeIt>
	void fillPoints(PointRange &points, NodeIt begin, NodeIt end) const {
		for (auto it = begin; it != end; ++it) {
			LatpLon ll = nodes.at(*it);
			boost::geometry::range::push_back(points, boost::geometry::make<Point>(ll.lon/10000000.0, ll.latp/10000000.0));
		}
	}
};

#endif //_OSM_STORE_H
