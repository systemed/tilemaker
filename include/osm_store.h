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
#include <boost/interprocess/managed_mapped_file.hpp>
#include <boost/interprocess/allocators/node_allocator.hpp>
#include <boost/container/vector.hpp>
#include <boost/container/scoped_allocator.hpp>
#include <boost/unordered_map.hpp>
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

//
// Internal data structures.
//

// node store
class NodeStore {
	using pair_t = std::pair<NodeID, LatpLon>;
	using pair_allocator_t = boost::interprocess::node_allocator<pair_t, mmap_file_t::segment_manager>;
	using map_t = boost::unordered_map<NodeID, LatpLon, std::hash<NodeID>, std::equal_to<NodeID>, pair_allocator_t>;
	
	map_t *mLatpLons;

public:

	void reopen(mmap_file_t &mmap_file)
	{
		mLatpLons = mmap_file.find_or_construct<map_t>("node_store")(mmap_file.get_segment_manager());
		mLatpLons->reserve(10000000);
	}

	// @brief Lookup a latp/lon pair
	// @param i OSM ID of a node
	// @return Latp/lon pair
	// @exception NotFound
	LatpLon const &at(NodeID i) const;

	// @brief Return whether a latp/lon pair is on the store.
	// @param i Any possible OSM ID
	// @return 1 if found, 0 otherwise
	// @note This function is named as count for consistent naming with stl functions.
	size_t count(NodeID i) const;

	// @brief Insert a latp/lon pair.
	// @param i OSM ID of a node
	// @param coord a latp/lon pair to be inserted
	// @invariant The OSM ID i must be larger than previously inserted OSM IDs of nodes
	//			  (though unnecessarily for current impl, future impl may impose that)
	void insert_back(NodeID i, LatpLon coord);

	// @brief Make the store empty
	void clear();

	size_t size();
};

// way store

class WayStore {

	template <typename T> using scoped_alloc_t = boost::container::scoped_allocator_adaptor<T>;
	using nodeid_allocator_t = boost::interprocess::node_allocator<NodeID, mmap_file_t::segment_manager>;
	using nodeid_vector_t = boost::container::vector<NodeID, scoped_alloc_t<nodeid_allocator_t>>;

	using pair_t = std::pair<const NodeID, nodeid_vector_t>;
	using pair_allocator_t = boost::interprocess::node_allocator<pair_t, mmap_file_t::segment_manager>;
	using map_t = boost::unordered_map<NodeID, nodeid_vector_t, std::hash<NodeID>, std::equal_to<NodeID>, scoped_alloc_t<pair_allocator_t>>;

	map_t *mNodeLists;

public:

	using const_iterator = nodeid_vector_t::const_iterator;

	void reopen(mmap_file_t &mmap_file)
	{
		mNodeLists = mmap_file.find_or_construct<map_t>("way_store")(mmap_file.get_segment_manager());
		mNodeLists->reserve(1000000);
	}

	// @brief Lookup a node list
	// @param i OSM ID of a way
	// @return A node list
	// @exception NotFound
	NodeList<nodeid_vector_t::const_iterator> at(WayID i) const;

	// @brief Return whether a node list is on the store.
	// @param i Any possible OSM ID
	// @return 1 if found, 0 otherwise
	// @note This function is named as count for consistent naming with stl functions.
	size_t count(WayID i) const;

	// @brief Insert a node list.
	// @param i OSM ID of a way
	// @param nodeVec a node vector to be inserted
	// @invariant The OSM ID i must be larger than previously inserted OSM IDs of ways
	//			  (though unnecessarily for current impl, future impl may impose that)
	template<typename Iterator>			  
	void insert_back(WayID i, Iterator begin, Iterator end) {
		mNodeLists->emplace(std::piecewise_construct,
			std::forward_as_tuple(i),
			std::forward_as_tuple(begin, end)); 
	}

	// @brief Make the store empty
	void clear();

	size_t size();
};

// relation store

class RelationStore {

	template <typename T> using scoped_alloc_t = boost::container::scoped_allocator_adaptor<T>;

	using wayid_allocator_t = boost::interprocess::node_allocator<WayID, mmap_file_t::segment_manager>;

	class wayid_vector_t
		: public  boost::container::vector<WayID, scoped_alloc_t<wayid_allocator_t>>
	{
	public:
			template<typename IteratorTuple, typename Allocator>
			wayid_vector_t(IteratorTuple init, Allocator &allocator)
				: boost::container::vector<WayID, scoped_alloc_t<wayid_allocator_t>>(init.first, init.second, allocator) 
			{ }
	};

	using relation_entry_t = std::pair<wayid_vector_t, wayid_vector_t>;
	using relation_allocator_t = boost::interprocess::node_allocator<relation_entry_t, mmap_file_t::segment_manager>;

	using pair_t = std::pair<WayID, relation_entry_t>;
	using pair_allocator_t = boost::interprocess::node_allocator<pair_t, mmap_file_t::segment_manager>;
	using map_t = boost::unordered_map<WayID, relation_entry_t, std::hash<WayID>, std::equal_to<WayID>, scoped_alloc_t<pair_allocator_t>>;

	map_t *mOutInLists;

public:

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
	void insert_front(WayID i, Iterator outerWayVec_begin, Iterator outerWayVec_end, Iterator innerWayVec_begin, Iterator innerWayVec_end) {
		mOutInLists->emplace(std::piecewise_construct,
			std::forward_as_tuple(i),
			std::forward_as_tuple(
				std::make_pair(outerWayVec_begin, outerWayVec_end), 
				std::make_pair(innerWayVec_begin, innerWayVec_end)));
	}

	void clear();

	size_t size();
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
	template< class Iterator >
	static std::reverse_iterator<Iterator> make_reverse_iterator(Iterator i)
	{
		return std::reverse_iterator<Iterator>(i);
	}		

	NodeStore nodes;
	WayStore ways;
	RelationStore relations;

	template <typename T> using scoped_alloc_t = boost::container::scoped_allocator_adaptor<T>;

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
	constexpr static std::size_t init_map_size = 64000000;
	std::size_t map_size = init_map_size;

	void remove_mmap_file();

	mmap_file_t create_mmap_file()
	{
		remove_mmap_file();
		return mmap_file_t(boost::interprocess::create_only, osm_store_filename.c_str(), init_map_size);
	}

	mmap_file_t open_mmap_file()
	{
		return mmap_file_t(boost::interprocess::open_only, osm_store_filename.c_str());
	}

	mmap_file_t mmap_file;

	void reopen() {
		nodes.reopen(mmap_file);
		ways.reopen(mmap_file);
		relations.reopen(mmap_file);
		
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

public:

	OSMStore(std::string const &osm_store_filename)
	   : osm_store_filename(osm_store_filename) 
	{ 
		mmap_file = create_mmap_file();
		perform_mmap_operation([&]() {
			reopen();
		});
	}

	~OSMStore()
	{
		remove_mmap_file();
	}

	// Store and retrieve ways/nodes and relations in the mmap file
	void nodes_insert_back(NodeID i, LatpLon coord) {
		perform_mmap_operation([&]() {
			nodes.insert_back(i, coord);
		});
	}

	LatpLon const &nodes_at(NodeID i) const {
		return nodes.at(i);
	}

	NodeList<WayStore::const_iterator> ways_at(WayID i) const {
		return ways.at(i);
	}

	void ways_insert_back(WayID i, const NodeVec &nodeVec) {
		perform_mmap_operation([&]() {
			ways.insert_back(i, nodeVec.begin(), nodeVec.end());
		});
	}

	WayList<RelationStore::const_iterator> relations_at(WayID i) const {
		return relations.at(i);
	}

	void relations_insert_front(WayID i, const WayVec &outerWayVec, const WayVec &innerWayVec) {
		relations.insert_front(i, outerWayVec.begin(), outerWayVec.end(), innerWayVec.begin(), innerWayVec.end());
	}

	// Get the currently allocated memory size in the mmap
	std::size_t getMemorySize() const { return map_size; }

	// Following methods are to store and retrieve the generated geometries in the mmap file
	using handle_t = mmap_file_t::handle_t;

	generated &osm() { return osm_generated; }
	generated &shp() { return shp_generated; }

	template<class T>
	T const &retrieve(handle_t handle) const {
		return *(static_cast<T const *>(mmap_file.get_address_from_handle(handle)));
	}

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
			store.linestring_store->resize(store.linestring_store->size() + 1);
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
				mmap::polygon_t::inners_type inners(polygon.inners().size(), result.get_allocator());
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
	void clear();

	void reportSize();

	// Relation -> MultiPolygon
	template<class WayIt>
	MultiPolygon wayListMultiPolygon(WayList<WayIt> wayList) const {
		MultiPolygon mp;
		if (wayList.outerBegin == wayList.outerEnd) { return mp; } // no outers so quit

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
		mergeMultiPolygonWays(wayList, outers, done, wayList.outerBegin, wayList.outerEnd);
		mergeMultiPolygonWays(wayList, inners, done, wayList.innerBegin, wayList.innerEnd);

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
	void mergeMultiPolygonWays(WayList<WayIt> &wayList,
		std::vector<NodeVec> &results, std::map<WayID,bool> &done, WayIt itBegin, WayIt itEnd) const {

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
							ot->insert(ot->end(), 
								make_reverse_iterator(nodes.end), 
								make_reverse_iterator(nodes.begin));
							joined=true; break;
						} else if (jLast==oFirst) {
							// prepend to the original
							ot->insert(ot->begin(), nodes.begin, nodes.end);
							joined=true; break;
						} else if (jFirst==oFirst) {
							// prepend reversed to the original
							ot->insert(ot->begin(), 
								make_reverse_iterator(nodes.end), 
								make_reverse_iterator(nodes.begin));
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

	MultiPolygon wayListMultiPolygon(WayID relId) const;

	MultiPolygon wayListMultiPolygon(const WayVec &outerWayVec, const WayVec &innerWayVec) const;

	///It is not really meaningful to try using a relation as a linestring. Not normally used but included
	///if Lua script attempts to do this.
	Linestring wayListLinestring(const WayVec &outerWayVec, const WayVec &innerWayVec) const;

	// Way -> Polygon
	template<class NodeIt>
	Polygon nodeListPolygon(NodeList<NodeIt> nodeList) const {
		Polygon poly;
		fillPoints(poly.outer(), nodeList.begin, nodeList.end);
		geom::correct(poly);
		return poly;
	}

	Polygon nodeListPolygon(WayID wayId) const;

	Polygon nodeListPolygon(const NodeVec &nodeVec) const;

	// Way -> Linestring
	template<class NodeIt>
	Linestring nodeListLinestring(NodeList<NodeIt> nodeList) const{
		Linestring ls;
		fillPoints(ls, nodeList.begin, nodeList.end);
		return ls;
	}

	Linestring nodeListLinestring(WayID wayId) const;

	Linestring nodeListLinestring(const NodeVec &nodeVec) const;

private:
	// helper
	template<class PointRange, class NodeIt>
	void fillPoints(PointRange &points, NodeIt begin, NodeIt end) const {
		for (auto it = begin; it != end; ++it) {
			LatpLon ll = nodes.at(*it);
			geom::range::push_back(points, geom::make<Point>(ll.lon/10000000.0, ll.latp/10000000.0));
		}
	}
};

#endif //_OSM_STORE_H
