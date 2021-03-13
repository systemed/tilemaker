/*! \file */ 
#ifndef _OSM_STORE_H
#define _OSM_STORE_H

#include <utility>
#include <vector>
#include <iostream>
#include "geomtypes.h"
#include "coordinates.h"
namespace geom = boost::geometry;

#include <boost/geometry/geometries/register/linestring.hpp>
#include <boost/interprocess/detail/move.hpp>
#include <boost/interprocess/managed_mapped_file.hpp>
#include <boost/interprocess/allocators/node_allocator.hpp>
#include <boost/interprocess/containers/string.hpp>
#include <boost/container/vector.hpp>
#include <boost/container/small_vector.hpp>
#include <boost/container/scoped_allocator.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/unordered_map.hpp>
#include <boost/filesystem.hpp>

#include <iterator> 
#include <cstddef>  
#include <type_traits>

template<typename T, typename A>
struct VarIntIterator
{
	using iterator_category = std::bidirectional_iterator_tag;
	using difference_type   = std::ptrdiff_t;
	using value_type        = T;
	using pointer           = value_type const*;  // or also value_type*
	using reference         = value_type const&;  // or also value_type&
	using unsigned_value_type  = typename std::make_unsigned<T>::type;

	using vector_type = boost::container::small_vector<uint8_t, 15, A>;
	using const_iterator = typename vector_type::const_iterator;

	VarIntIterator(const_iterator ptr, value_type value = 0)
		: value(value), begin_ptr(ptr), m_ptr(ptr)
	{ }

	value_type operator*() const {
		return value + decode(varint_read(m_ptr));
	}

	VarIntIterator& operator--() {
		--m_ptr;
		while(m_ptr != begin_ptr && (*(m_ptr - 1) & 0x80))
			--m_ptr;

		value -= decode(varint_read(m_ptr));
		return *this;
	}

	VarIntIterator operator--(int) {
		VarIntIterator tmp = *this;
		--(*this);
		return tmp;
	}

	VarIntIterator& operator++() {
		value += decode(varint_read(m_ptr));
		while((*m_ptr & 0x80) != 0)
			++m_ptr;
		++m_ptr;
		return *this;
	}

	VarIntIterator operator++(int) {
		VarIntIterator tmp = *this;
		++(*this);
		return tmp;
	}

	friend bool operator== (const VarIntIterator& a, const VarIntIterator& b) { return a.m_ptr == b.m_ptr; };
	friend bool operator!= (const VarIntIterator& a, const VarIntIterator& b) { return a.m_ptr != b.m_ptr; };

	const_iterator ptr() const { return m_ptr; }

protected:
    static inline value_type decode(unsigned_value_type value) {
    	return static_cast<value_type>((value >> 1) ^ -(value & 1));
    }

	template<class FwdIt>
	static inline value_type varint_read(FwdIt first)
	{
		value_type value = 0;
		value_type factor = 1;
		while((*first & 0x80) != 0)
		{
			value += (*first++ & 0x7f) * factor;
			factor *= 128;
		}
		value += *first++ * factor;
		return value;
	}

	value_type value;
	const_iterator begin_ptr;
	const_iterator m_ptr;
};

/** Storage for delta-encoded compressed node lists */
template<typename T, typename A>
class VarIntVector
{
public:

	using value_type = T;
	using signed_value_type = typename std::make_signed<T>::type;
	using const_iterator = VarIntIterator<signed_value_type, A>;
	using allocator_type = A;

	using vector_type = boost::container::small_vector<uint8_t, 15, A>;

	VarIntVector(A alloc = A())
		: values(0, typename vector_type::allocator_type(alloc))
	{ }

	template<typename InputIt>
	VarIntVector(InputIt begin, InputIt end, A const &alloc = A())
		: values(0, typename vector_type::allocator_type(alloc))
	{ 
		values.reserve(std::distance(begin, end));
		last_value = 0;
		while(begin != end) { 
			value_type value = *begin;
        	push_value(encode(value - last_value));
	        last_value = value;
			++begin;
		}
	}

    VarIntIterator<signed_value_type, A> cbegin() const { 
		return VarIntIterator<signed_value_type, A>(values.cbegin()); 
	}
    VarIntIterator<signed_value_type, A> cend() const {
        return VarIntIterator<signed_value_type, A>(values.cend(), last_value);
        /* auto result = VarIntIterator<signed_value_type, A>(values.cbegin());
        while(result.ptr() != values.cend())
            result++;
        return result;  */
    }

    void push_back(value_type value)
    {
		//auto last_value = *std::prev(cend());
        push_value(encode(value - last_value));
        last_value = value;
    }

    std::size_t size() const { return values.size(); }
	bool empty() const { return values.empty(); }

	value_type front() const { return *cbegin(); }
	value_type back() const { return last_value; }

private:
	value_type encode(signed_value_type value) {
        return (value << 1) ^ (value >> (std::numeric_limits<value_type>::digits - 1));
    }

    void push_value(value_type value)
    {
        while(value > 127)
        {
            values.push_back(static_cast<uint8_t>(0x80 | value));
            value /= 128;
        }
        values.push_back(static_cast<uint8_t>(value));
    }

	vector_type values;
	value_type last_value;
};

//
// Views of data structures.
//

template<class NodeIt>
struct NodeList {
	NodeIt begin;
	NodeIt end;
};

NodeList<NodeVec::const_iterator> makeNodeList(const NodeVec &nodeVec);

template<class NodeIt>
static inline bool isClosed(NodeList<NodeIt> const &way) {
	return *way.begin == *std::prev(way.end);
}

template<class WayIt>
struct WayList {
	WayIt outerBegin;
	WayIt outerEnd;
	WayIt innerBegin;
	WayIt innerEnd;
};

WayList<WayVec::const_iterator> makeWayList( const WayVec &outerWayVec, const WayVec &innerWayVec);

using mmap_file_t = boost::interprocess::managed_mapped_file;

enum NodeStoreType { NodeStoreType_Compact, NodeStoreType_Normal };

//
// Internal data structures.
//
class NodeStore
{
	using nodestore_pair_t = std::pair<NodeID, LatpLon>;
	using nodestore_pair_allocator_t = boost::interprocess::node_allocator<nodestore_pair_t, mmap_file_t::segment_manager>;
	using map_t = boost::unordered_map<NodeID, LatpLon, std::hash<NodeID>, std::equal_to<NodeID>, nodestore_pair_allocator_t>;

	map_t *mLatpLons;

public:

	NodeStore()
	{ }

	// @brief reopen the datastructure after size of mmap file has changed
	void reopen(mmap_file_t &mmap_file)
	{
		if(*mmap_file.get_segment_manager()->find_or_construct<NodeStoreType>("node_store_type")(NodeStoreType_Normal) != NodeStoreType_Normal) {
			throw std::runtime_error("Nodestore generated as compact");
		}
		
		mLatpLons = mmap_file.find_or_construct<map_t>("node_store")(mmap_file.get_segment_manager());
	}

	// @brief prereserve the specified number of items
	void reserve(uint nodes) {
		mLatpLons->reserve(nodes);
	}

	// @brief Lookup a latp/lon pair
	// @param i OSM ID of a node
	// @return Latp/lon pair
	// @exception NotFound
	LatpLon const &at(NodeID i) const {
		return mLatpLons->at(i);
	}

	// @brief Return the number of stored items
	size_t size() const { return mLatpLons->size(); }

	// @brief Insert a latp/lon pair.
	// @param i OSM ID of a node
	// @param coord a latp/lon pair to be inserted
	// @invariant The OSM ID i must be larger than previously inserted OSM IDs of nodes
	//			  (though unnecessarily for current impl, future impl may impose that)
	void insert_back(NodeID i, LatpLon coord) {
		mLatpLons->emplace(i, coord);
	}

	// @brief Make the store empty
	void clear() { mLatpLons->clear(); } 
};


class NodeStoreCompact
{
	using nodestore_allocator_t = boost::interprocess::node_allocator<LatpLon, mmap_file_t::segment_manager>;
	using vector_t = std::vector<LatpLon, nodestore_allocator_t>;

	vector_t *mLatpLons;

public:

	NodeStoreCompact() 
	{ }

	// @brief reopen the datastructure after size of mmap file has changed
	void reopen(mmap_file_t &mmap_file)
	{
		if(*mmap_file.get_segment_manager()->find_or_construct<NodeStoreType>("node_store_type")(NodeStoreType_Compact) != NodeStoreType_Compact) {
			throw std::runtime_error("Nodestore not generated as compact");
		}
		
		mLatpLons = mmap_file.find_or_construct<vector_t>("node_store")(mmap_file.get_segment_manager());
	}

	// @brief Lookup a latp/lon pair
	// @param i OSM ID of a node
	// @return Latp/lon pair
	// @exception NotFound
	LatpLon const &at(NodeID i) const {
		return mLatpLons->at(i);
	}

	// @brief prereserve the specified number of items
	void reserve(uint nodes) {
		if(nodes > mLatpLons->max_size()) {
			throw boost::interprocess::bad_alloc();
		}

		std::cout << "Resize node store: " << nodes << ", max size: " << mLatpLons->max_size() << std::endl;
		mLatpLons->reserve(nodes);
	}

	// @brief Insert a latp/lon pair.
	// @param i OSM ID of a node
	// @param coord a latp/lon pair to be inserted
	// @invariant The OSM ID i must be larger than previously inserted OSM IDs of nodes
	//			  (though unnecessarily for current impl, future impl may impose that)
	void insert_back(NodeID i, LatpLon coord) {
		if(i >= mLatpLons->capacity()) {
			throw std::out_of_range("Failed to store node " + std::to_string(i) + ", index out of range");
		}

		mLatpLons->resize(std::max<size_t>(i + 1, mLatpLons->size()));
		(*mLatpLons)[i] = coord;
	}

	// @brief Return the number of stored items
	size_t size() const { return mLatpLons->size(); }

	// @brief Make the store empty
	void clear() { 
		//std::fill(mLatpLons->begin(), mLatpLons->end(), LatpLon());
		mLatpLons->resize(0);
	}
};

// way store
class WayStore {

public:
	template <typename T> using scoped_alloc_t = boost::container::scoped_allocator_adaptor<T>;
	using nodeid_allocator_t = boost::interprocess::node_allocator<uint8_t, mmap_file_t::segment_manager>;
	using nodeid_vector_t = VarIntVector<NodeID, scoped_alloc_t<nodeid_allocator_t>>;

	using pair_t = std::pair<const NodeID, nodeid_vector_t>;
	using pair_allocator_t = boost::interprocess::node_allocator<pair_t, mmap_file_t::segment_manager>;
	using map_t = boost::unordered_map<NodeID, nodeid_vector_t, std::hash<NodeID>, std::equal_to<NodeID>, scoped_alloc_t<pair_allocator_t>>;

	using const_iterator = typename nodeid_vector_t::const_iterator;

	void reopen(mmap_file_t &mmap_file)
	{
		mNodeLists = mmap_file.find_or_construct<map_t>("way_store")(mmap_file.get_segment_manager());
	}

	void reserve(uint ways) {
		mNodeLists->reserve(ways);
	}

	// @brief Lookup a node list
	// @param i OSM ID of a way
	// @return A node list
	// @exception NotFound
	NodeList<const_iterator> at(WayID i) const {
		try {
			const auto &way = mNodeLists->at(i);
			return { way.cbegin(), way.cend() };
		}
		catch (std::out_of_range &err){
			throw std::out_of_range(std::string("Could not find way ") + std::to_string(i));
		}
	}

	// @brief Return whether a node list is on the store.
	// @param i Any possible OSM ID
	// @return 1 if found, 0 otherwise
	// @note This function is named as count for consistent naming with stl functions.
	size_t count(WayID i) const {
		return mNodeLists->count(i);
	}

	// @brief Insert a node list.
	// @param i OSM ID of a way
	// @param nodeVec a node vector to be inserted
	// @invariant The OSM ID i must be larger than previously inserted OSM IDs of ways
	//			  (though unnecessarily for current impl, future impl may impose that)
	template<typename Iterator>			  
	nodeid_vector_t const &insert_back(WayID i, Iterator begin, Iterator end) {
		auto result = mNodeLists->emplace(std::piecewise_construct,
			std::forward_as_tuple(i),
			std::forward_as_tuple(begin, end));
		return result.first->second;
	}

	// @brief Make the store empty
	void clear() { mNodeLists->clear(); }

	std::size_t size() const { return mNodeLists->size(); }

private:	
	map_t *mNodeLists;
};

// relation store

class RelationStore {

public:	
	template <typename T> using scoped_alloc_t = boost::container::scoped_allocator_adaptor<T>;

	//using wayid_allocator_t = boost::interprocess::node_allocator<WayID, mmap_file_t::segment_manager>;
	using wayid_allocator_t = boost::interprocess::node_allocator<uint8_t, mmap_file_t::segment_manager>;

	class wayid_vector_t
		: public VarIntVector<WayID, scoped_alloc_t<wayid_allocator_t>>
	{
	public:
			template<typename IteratorTuple, typename Allocator>
			wayid_vector_t(IteratorTuple init, Allocator &allocator)
				: VarIntVector<WayID, scoped_alloc_t<wayid_allocator_t>>(init.first, init.second, allocator) 
			{ }
	};

	using relation_entry_t = std::pair<wayid_vector_t, wayid_vector_t>;
	using relation_allocator_t = boost::interprocess::node_allocator<relation_entry_t, mmap_file_t::segment_manager>;

	using pair_t = std::pair<WayID, relation_entry_t>;
	using pair_allocator_t = boost::interprocess::node_allocator<pair_t, mmap_file_t::segment_manager>;
	using map_t = boost::unordered_map<WayID, relation_entry_t, std::hash<WayID>, std::equal_to<WayID>, scoped_alloc_t<pair_allocator_t>>;

	using const_iterator = wayid_vector_t::const_iterator;

	void reopen(mmap_file_t &mmap_file)
	{
		mOutInLists = mmap_file.find_or_construct<map_t>("relation_store")(mmap_file.get_segment_manager());
	}

	// @brief Lookup a way list
	// @param i Pseudo OSM ID of a relational way
	// @return A way list
	// @exception NotFound
	WayList<const_iterator> at(WayID i) const;

	// @brief Return whether a way list is on the store.
	// @param i Any possible OSM ID
	// @return 1 if found, 0 otherwise
	// @note This function is named as count for consistent naming with stl functions.
	size_t count(WayID i) const;

	// @brief Insert a way list.
	// @param i Pseudo OSM ID of a relation
	// @param outerWayVec A outer way vector to be inserted
	// @param innerWayVec A inner way vector to be inserted
	// @invariant The pseudo OSM ID i must be smaller than previously inserted pseudo OSM IDs of relations
	//			  (though unnecessarily for current impl, future impl may impose that)
	template<class Iterator>
	relation_entry_t const &insert_front(WayID i, Iterator outerWayVec_begin, Iterator outerWayVec_end, Iterator innerWayVec_begin, Iterator innerWayVec_end) {
		auto result = mOutInLists->emplace(std::piecewise_construct,
			std::forward_as_tuple(i),
			std::forward_as_tuple(
				std::make_pair(outerWayVec_begin, outerWayVec_end), 
				std::make_pair(innerWayVec_begin, innerWayVec_end)));
		return result.first->second;
	}

	void clear();

	size_t size() const;

private: 	
	map_t *mOutInLists;
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
	template <typename T> using scoped_alloc_t = boost::container::scoped_allocator_adaptor<T>;
	using string_allocator_t = boost::interprocess::node_allocator<char, mmap_file_t::segment_manager>;
	using mmap_string_t = boost::interprocess::basic_string<char, std::char_traits<char>, string_allocator_t>;

	using tag_allocator_t = boost::interprocess::node_allocator<std::pair<mmap_string_t, mmap_string_t>, mmap_file_t::segment_manager>;
	using tag_map_t = boost::container::flat_map<mmap_string_t, mmap_string_t, std::less<mmap_string_t>, scoped_alloc_t<tag_allocator_t> >;

protected:	
	template< class Iterator >
	static std::reverse_iterator<Iterator> make_reverse_iterator(Iterator i)
	{
		return std::reverse_iterator<Iterator>(i);
	}		

	WayStore ways;
	RelationStore relations;

	struct PbfNodeEntry {
		NodeID nodeId;
		LatpLon node;
		tag_map_t tags;

		template<class Alloc>
		PbfNodeEntry(NodeID nodeId, LatpLon node, tag_map_t tags, Alloc alloc = Alloc())
			: nodeId(nodeId), node(node), tags(tags, alloc) 
		{ }
	};

	struct PbfWayEntry {
		WayID wayId;
		mmap_file_t::handle_t nodeVecHandle;
		tag_map_t tags;

		template<class Alloc>
		PbfWayEntry(WayID wayId, mmap_file_t::handle_t nodeVecHandle, tag_map_t tags, Alloc alloc = Alloc())
			: wayId(wayId), nodeVecHandle(nodeVecHandle), tags(tags, alloc) 
		{ }
	};

	struct PbfRelationEntry {
		int64_t relationId;
		mmap_file_t::handle_t relationHandle;
		tag_map_t tags;
		
		template<class Alloc>
		PbfRelationEntry(int64_t relationId, mmap_file_t::handle_t relationHandle, tag_map_t tags, Alloc alloc = Alloc())
			: relationId(relationId), relationHandle(relationHandle), tags(tags, alloc) 
		{ }
	};

	using pbf_node_allocator_t = boost::interprocess::node_allocator<PbfNodeEntry, mmap_file_t::segment_manager>;
	using pbf_node_entries_t = std::deque< PbfNodeEntry, pbf_node_allocator_t>;
	pbf_node_entries_t *node_entries;

	using pbf_way_allocator_t = boost::interprocess::node_allocator<PbfWayEntry, mmap_file_t::segment_manager>;
	using pbf_way_entries_t = std::deque< PbfWayEntry, pbf_way_allocator_t>;
   	pbf_way_entries_t *way_entries;

	using pbf_relation_allocator_t = boost::interprocess::node_allocator<PbfRelationEntry, mmap_file_t::segment_manager>;
	using pbf_relation_entries_t = std::deque< PbfRelationEntry, pbf_relation_allocator_t>;
   	pbf_relation_entries_t *relation_entries;

	using point_allocator_t = boost::interprocess::node_allocator<Point, mmap_file_t::segment_manager>;
	using point_store_t = std::deque<Point, point_allocator_t>;

	using linestring_t = mmap::linestring_t;
	using linestring_allocator_t = boost::interprocess::node_allocator<linestring_t, mmap_file_t::segment_manager>;
	using linestring_store_t = std::deque<linestring_t, scoped_alloc_t<linestring_allocator_t>>;

	using multi_polygon_store_t = std::deque<mmap::multi_polygon_t, 
		  scoped_alloc_t<boost::interprocess::node_allocator<mmap::multi_polygon_t, mmap_file_t::segment_manager>>>;

	struct generated {
		point_store_t *points_store;
		linestring_store_t *linestring_store;
		multi_polygon_store_t *multi_polygon_store;
	};

	generated osm_generated;
	generated shp_generated;

	std::string osm_store_filename;
	enum { init_map_size = 1024000000 };
	std::size_t map_size = init_map_size;

	void remove_mmap_file() {
		boost::filesystem::remove(osm_store_filename);
	}

	mmap_file_t create_mmap_file(bool erase)
	{
		if(erase) {
			remove_mmap_file();
			return mmap_file_t(boost::interprocess::create_only, osm_store_filename.c_str(), init_map_size);
		}
		return open_mmap_file();
	}

	mmap_file_t open_mmap_file()
	{
		return mmap_file_t(boost::interprocess::open_only, osm_store_filename.c_str());
	}

	mmap_file_t mmap_file;
	bool erase;						// Erase mmap file at startup/exit ?

	void reopen() {
		impl_reopen(mmap_file);
		ways.reopen(mmap_file);
		relations.reopen(mmap_file);
		
		node_entries = mmap_file.find_or_construct<pbf_node_entries_t>
			("pbf_node_entries")(mmap_file.get_segment_manager());
		way_entries = mmap_file.find_or_construct<pbf_way_entries_t>
			("pbf_way_entries")(mmap_file.get_segment_manager());
		relation_entries = mmap_file.find_or_construct<pbf_relation_entries_t>
			("pbf_relation_entries")(mmap_file.get_segment_manager());

		osm_generated.points_store = mmap_file.find_or_construct<point_store_t>
			("osm_point_store")(mmap_file.get_segment_manager());
		osm_generated.linestring_store = mmap_file.find_or_construct<linestring_store_t>
			("osm_linestring_store")(mmap_file.get_segment_manager());
		osm_generated.multi_polygon_store = mmap_file.find_or_construct<multi_polygon_store_t>
			("osm_multi_polygon_store")(mmap_file.get_segment_manager()); 

		shp_generated.points_store = mmap_file.find_or_construct<point_store_t>
			("shp_point_store")(mmap_file.get_segment_manager());
		shp_generated.linestring_store = mmap_file.find_or_construct<linestring_store_t>
			("shp_linestring_store")(mmap_file.get_segment_manager());
		shp_generated.multi_polygon_store = mmap_file.find_or_construct<multi_polygon_store_t>
			("shp_multi_polygon_store")(mmap_file.get_segment_manager());  
	}

	template<typename Func>
	void perform_mmap_operation(Func func) {
		while(true) {
			try {
				func();
				return;
			} catch(boost::interprocess::bad_alloc &e) {
				
				mmap_file = mmap_file_t();

				// Double the size of the mmap size
				std::cout << "Resizing osm store to size: " << (2 * map_size / 1000000) << "M                " << std::endl;
				boost::interprocess::managed_mapped_file::grow(osm_store_filename.c_str(), map_size);
				map_size = map_size * 2;

				mmap_file = open_mmap_file();
				reopen();
			}
		}
	}

	NodeList<WayStore::const_iterator> ways_at(WayID i) const {
		return ways.at(i);
	}

	WayList<RelationStore::const_iterator> relations_at(WayID i) const {
		return relations.at(i);
	}

	// Implementation of serveral functions
	virtual void impl_nodes_insert_back(NodeID i, LatpLon coord) = 0;
	virtual LatpLon const &impl_nodes_at(NodeID i) const = 0;
	virtual void impl_reopen(mmap_file_t &mmap_file) = 0;

public:

	OSMStore(std::string const &osm_store_filename, bool erase = true)
		: osm_store_filename(osm_store_filename) 
		, mmap_file(create_mmap_file(erase)), erase(erase)
	{ }

	virtual ~OSMStore()
	{
		if(erase) 
			remove_mmap_file();
	}

	void save(std::string const &filename) { 
		mmap_file = mmap_file_t();
		
		boost::filesystem::rename(osm_store_filename, filename);
		boost::interprocess::managed_mapped_file::shrink_to_fit(filename.c_str());
	}

	// Following methods are to store and retrieve the generated geometries in the mmap file
	using handle_t = mmap_file_t::handle_t;

	template<class T>
	T const &retrieve(handle_t handle) const {
		return *(static_cast<T const *>(mmap_file.get_address_from_handle(handle)));
	}

	// Store and retrieve ways/nodes and relations in the mmap file
	void nodes_insert_back(NodeID i, LatpLon coord) {
		perform_mmap_operation([&]() {
			impl_nodes_insert_back(i, coord);
		});
	}

	LatpLon const &nodes_at(NodeID i) const {
		return impl_nodes_at(i);
	}

	handle_t ways_insert_back(WayID i, const NodeVec &nodeVec) {
		handle_t result;
		perform_mmap_operation([&]() {
			auto const &way = ways.insert_back(i, nodeVec.begin(), nodeVec.end());
			result = mmap_file.get_handle_from_address(&way);
		});
		return result;
	}

	template<class WayIt>
	Polygon nodeListPolygon(WayIt begin, WayIt end) const {
		Polygon poly;
		fillPoints(poly.outer(), begin, end);
		geom::correct(poly);
		return poly;
	}

	// Way -> Linestring
	template<class WayIt>
	Linestring nodeListLinestring(WayIt begin, WayIt end) const {
		Linestring ls;
		fillPoints(ls, begin, end);
		return ls;
	}

	handle_t relations_insert_front(WayID i, const WayVec &outerWayVec, const WayVec &innerWayVec) {
		handle_t result;
		perform_mmap_operation([&]() {
			auto const &relation = relations.insert_front(i, outerWayVec.begin(), outerWayVec.end(), innerWayVec.begin(), innerWayVec.end());
			result = mmap_file.get_handle_from_address(&relation);
		});
		return result;
	}

	// Store and retrieve pbf entries
	std::size_t total_pbf_node_entries() const { return node_entries->size(); };
	PbfNodeEntry const &pbf_node_entry(std::size_t i) const { return node_entries->at(i); }

	template<class T>
	void pbf_store_node_entry(NodeID nodeId, LatpLon node, T const &tags) {
		perform_mmap_operation([&]() {
			tag_map_t store_tags(node_entries->get_allocator());
			for(auto const &i: tags) {
				store_tags.emplace(
					std::piecewise_construct,
					std::forward_as_tuple(mmap_string_t(i.first.begin(), i.first.end(), node_entries->get_allocator())), 
					std::forward_as_tuple(mmap_string_t(i.second.begin(), i.second.end(), node_entries->get_allocator()))); 
			} 
			node_entries->emplace_back(nodeId, node, boost::interprocess::move(store_tags), node_entries->get_allocator());
		});
	}

	std::size_t total_pbf_way_entries() const { return way_entries->size(); };
	PbfWayEntry const &pbf_way_entry(std::size_t i) const { return way_entries->at(i); }

	template<class T>
	void pbf_store_way_entry(WayID wayId, handle_t handle, T const &tags) {
		perform_mmap_operation([&]() {
			tag_map_t store_tags(way_entries->get_allocator());
			for(auto const &i: tags) {
				store_tags.emplace(
					std::piecewise_construct,
					std::forward_as_tuple(mmap_string_t(i.first.begin(), i.first.end(), node_entries->get_allocator())), 
					std::forward_as_tuple(mmap_string_t(i.second.begin(), i.second.end(), node_entries->get_allocator()))); 
			}
			way_entries->emplace_back(wayId, handle, boost::interprocess::move(store_tags), way_entries->get_allocator());
		});
	}

	std::size_t total_pbf_relation_entries() const { return relation_entries->size(); };
	PbfRelationEntry const &pbf_relation_entry(std::size_t i) const { return relation_entries->at(i); }

	template<class T>
	void pbf_store_relation_entry(int64_t relationId, handle_t handle, T const &tags) {
		perform_mmap_operation([&]() {
			tag_map_t store_tags(relation_entries->get_allocator());
			for(auto const &i: tags) {
				store_tags.emplace(
					std::piecewise_construct,
					std::forward_as_tuple(mmap_string_t(i.first.begin(), i.first.end(), node_entries->get_allocator())), 
					std::forward_as_tuple(mmap_string_t(i.second.begin(), i.second.end(), node_entries->get_allocator()))); 
			}
			relation_entries->emplace_back(relationId, handle, boost::interprocess::move(store_tags), way_entries->get_allocator());
		});
	}

	// Get the currently allocated memory size in the mmap
	std::size_t getMemorySize() const { return map_size; }

	generated &osm() { return osm_generated; }
	generated &shp() { return shp_generated; }


	template<typename T>
	handle_t store_point(generated &store, T const &input) {
		perform_mmap_operation([&]() {
			store.points_store->emplace_back(input.x(), input.y());		   
		});

		return mmap_file.get_handle_from_address(&store.points_store->back());
	}


	template<typename Input>
	handle_t store_linestring(generated &store, Input const &src)
	{
		perform_mmap_operation([&]() {
			store.linestring_store->emplace_back();
			boost::geometry::assign(store.linestring_store->back(), src);
		});

		return mmap_file.get_handle_from_address(&store.linestring_store->back());
	}

	template<typename Input>
	handle_t store_multi_polygon(generated &store, Input const &src)
	{
		 perform_mmap_operation([&]() {
			 mmap::multi_polygon_t result(store.multi_polygon_store->get_allocator());
			 result.reserve(src.size());

			for(auto const &polygon: src) {
				mmap::polygon_t::inners_type inners(result.get_allocator());
				inners.resize(polygon.inners().size());

				mmap::polygon_t::ring_type outer(result.get_allocator());

				// Copy the outer ring
				boost::geometry::assign(outer, polygon.outer());

				// Store the inner rings
				for(std::size_t i = 0; i < polygon.inners().size(); ++i) {
					boost::geometry::assign(inners[i], polygon.inners()[i]);
				} 
			
				result.emplace_back(outer, inners);
			}

			store.multi_polygon_store->push_back(result);
		});

		return mmap_file.get_handle_from_address(&store.multi_polygon_store->back());
	}

	// @brief Make the store empty
	virtual void clear() = 0;

	virtual void reportSize() const = 0;


	// Relation -> MultiPolygon
	template<class WayIt>
	MultiPolygon wayListMultiPolygon(WayIt outerBegin, WayIt outerEnd, WayIt innerBegin, WayIt innerEnd) const {
		MultiPolygon mp;
		if (outerBegin == outerEnd) { return mp; } // no outers so quit

		// Assemble outers
		// - Any closed polygons are added as-is
		// - Linestrings are joined to existing linestrings with which they share a start/end
		// - If no matches can be found, then one linestring is added (to 'attract' others)
		// - The process is rerun until no ways are left
		// There's quite a lot of copying going on here - could potentially be addressed
		std::vector<NodeVec> outers;
		std::vector<NodeVec> inners;
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
		for (auto ot = outers.begin(); ot != outers.end(); ot++) {
			Polygon poly;
			fillPoints(poly.outer(), ot->begin(), ot->end());
			for (auto it = filledInners.begin(); it != filledInners.end(); ++it) {
				if (geom::within(*it, poly.outer())) { poly.inners().emplace_back(*it); }
			}
			mp.emplace_back(move(poly));
		}

		// fix winding
		geom::correct(mp);
		return mp;
	}

	template<class WayIt>
	void mergeMultiPolygonWays(std::vector<NodeVec> &results, std::map<WayID,bool> &done, WayIt itBegin, WayIt itEnd) const {

		int added;
		do {
			added = 0;
			for (auto it = itBegin; it != itEnd; ++it) {
				if (done[*it]) { continue; }
				auto way = ways.at(*it);
				if (isClosed(way)) {
					// if start==end, simply add it to the set
					results.emplace_back(NodeVec(way.begin, way.end));
					added++;
					done[*it] = true;
				} else {
					// otherwise, can we find a matching outer to append it to?
					bool joined = false;
					auto nodes = ways.at(*it);
					NodeID jFirst = *nodes.begin;
					NodeID jLast  = *(std::prev(nodes.end));
					for (auto ot = results.begin(); ot != results.end(); ot++) {
						NodeID oFirst = ot->front();
						NodeID oLast  = ot->back();
						if (jFirst==jLast) continue; // don't join to already-closed ways
						else if (oLast==jFirst) {
							// append to the original
							ot->insert(ot->end(), nodes.begin, nodes.end);
							joined=true; break;
						} else if (oLast==jLast) {
							// append reversed to the original
							NodeVec tmp(nodes.begin, nodes.end);
                            ot->insert(ot->end(),
                            	make_reverse_iterator(tmp.end()),
                                make_reverse_iterator(tmp.begin()));
							joined=true; break;
						} else if (jLast==oFirst) {
							// prepend to the original
							ot->insert(ot->begin(), nodes.begin, nodes.end);
							joined=true; break;
						} else if (jFirst==oFirst) {
							NodeVec tmp(nodes.begin, nodes.end);
                            ot->insert(ot->begin(),
                            	make_reverse_iterator(tmp.end()),
                                make_reverse_iterator(tmp.begin()));
							joined=true; break;
						}
					}
					if (joined) {
						added++;
						done[*it] = true;
					}
				}
			}
			// If nothing was added, then 'seed' it with a remaining unallocated way
			if (added==0) {
				for (auto it = itBegin; it != itEnd; ++it) {
					if (done[*it]) { continue; }
					auto way = ways.at(*it);
					results.emplace_back(NodeVec(way.begin, way.end));
					added++;
					done[*it] = true;
					break;
				}
			}
		} while (added>0);
	};

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


private:
	// helper
	template<class PointRange, class NodeIt>
	void fillPoints(PointRange &points, NodeIt begin, NodeIt end) const {
		for (auto it = begin; it != end; ++it) {
			LatpLon ll = nodes_at(*it);
			geom::range::push_back(points, geom::make<Point>(ll.lon/10000000.0, ll.latp/10000000.0));
		}
	}
};

template<class NodeStoreT = NodeStoreCompact>
class OSMStoreImpl
	: public OSMStore
{
	NodeStoreT nodes;

	void impl_nodes_insert_back(NodeID i, LatpLon coord) override {
		nodes.insert_back(i, coord);
	};

	LatpLon const &impl_nodes_at(NodeID i) const override {
		try {
			return nodes.at(i);
		}
		catch (std::out_of_range &err) {
			throw std::out_of_range("Could not find node " + std::to_string(i));
		}
	};

	void impl_reopen(mmap_file_t &mmap_file) override {
		nodes.reopen(mmap_file);
	}

public:
	OSMStoreImpl(std::string const &osm_store_filename, uint osm_store_nodes, uint osm_store_ways, bool erase_at_startup)
	   : OSMStore(osm_store_filename, erase_at_startup) 
	{
		reopen();

		perform_mmap_operation([&]() {
			nodes.reserve(osm_store_nodes);
			ways.reserve(osm_store_ways);
		});
	}

	void clear() override {
		nodes.clear();
		ways.clear();
		relations.clear();
	} 

	void reportSize() const override {
		std::cout << "Stored " << nodes.size() << " nodes, " << ways.size() << " ways, " << relations.size() << " relations" << std::endl;
		std::cout << "Shape points: " << shp_generated.points_store->size() << ", lines: " << shp_generated.linestring_store->size() << ", polygons: " << shp_generated.multi_polygon_store->size() << std::endl;
		std::cout << "Generated points: " << osm_generated.points_store->size() << ", lines: " << osm_generated.linestring_store->size() << ", polygons: " << osm_generated.multi_polygon_store->size() << std::endl;
	}
};

#endif //_OSM_STORE_H
