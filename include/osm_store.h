/*! \file */ 
#ifndef _OSM_STORE_H
#define _OSM_STORE_H

#include <utility>
#include <vector>
#include <iostream>
#include "geomtypes.h"
#include "coordinates.h"
#include "tsl/sparse_map.h"
namespace geom = boost::geometry;

#include <boost/interprocess/managed_mapped_file.hpp>
#include <boost/interprocess/allocators/node_allocator.hpp>
#include <boost/unordered_map.hpp>
#include <boost/container/vector.hpp>
#include <boost/container/scoped_allocator.hpp>

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

using mmap_file_ptr = std::shared_ptr<boost::interprocess::managed_mapped_file>;

//
// Internal data structures.
//

// node store
class NodeStore {
	using pair_t = std::pair<NodeID, LatpLon>;
	using pair_allocator_t = boost::interprocess::node_allocator<pair_t, boost::interprocess::managed_mapped_file::segment_manager, 131072>;
	using map_t = boost::unordered_map<NodeID, LatpLon, std::hash<NodeID>, std::equal_to<NodeID>, pair_allocator_t>;
	
	map_t *mLatpLons;

public:

	void reopen(mmap_file_ptr mmap_file)
	{
       	mLatpLons = mmap_file->find_or_construct<map_t>("node_store")(mmap_file->get_segment_manager());
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
	//            (though unnecessarily for current impl, future impl may impose that)
	void insert_back(NodeID i, LatpLon coord);

	// @brief Make the store empty
	void clear();

	size_t size();
};

// way store

class WayStore {

    template <typename T> using scoped_alloc_t = boost::container::scoped_allocator_adaptor<T>;
	using nodeid_allocator_t = boost::interprocess::node_allocator<NodeID, boost::interprocess::managed_mapped_file::segment_manager>;
    using nodeid_vector_t = boost::container::vector<NodeID, scoped_alloc_t<nodeid_allocator_t>>;

	using pair_t = std::pair<NodeID, nodeid_vector_t>;
	using pair_allocator_t = boost::interprocess::node_allocator<pair_t, boost::interprocess::managed_mapped_file::segment_manager>;
	using map_t = boost::unordered_map<NodeID, nodeid_vector_t, std::hash<NodeID>, std::equal_to<NodeID>, scoped_alloc_t<pair_allocator_t>>;

	map_t *mNodeLists;

public:

	using const_iterator = nodeid_vector_t::const_iterator;

	void reopen(mmap_file_ptr mmap_file)
	{
       	mNodeLists = mmap_file->find_or_construct<map_t>("way_store")(mmap_file->get_segment_manager());
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
	//            (though unnecessarily for current impl, future impl may impose that)
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
typedef std::vector<WayID>::const_iterator RelationStoreIterator;

class RelationStore {

	tsl::sparse_map<WayID, const std::pair<const std::vector<WayID>, const std::vector<WayID> > > mOutInLists;

public:
	// @brief Lookup a way list
	// @param i Pseudo OSM ID of a relational way
	// @return A way list
	// @exception NotFound
	WayList<RelationStoreIterator> at(WayID i) const;

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
	//            (though unnecessarily for current impl, future impl may impose that)
	void insert_front(WayID i, const WayVec &outerWayVec, const WayVec &innerWayVec);

	// @brief Make the store empty
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


	constexpr static char const *osm_store_filename = "osm_store.dat";
	constexpr static std::size_t init_map_size = 1000000;
	std::size_t map_size = init_map_size;

	static void remove_mmap_file();

	static mmap_file_ptr create_mmap_file()
	{
		remove_mmap_file();
      	return std::make_shared<boost::interprocess::managed_mapped_file>(
			boost::interprocess::create_only, osm_store_filename, init_map_size);
	}

	static mmap_file_ptr open_mmap_file()
	{
      	return std::make_shared<boost::interprocess::managed_mapped_file>(
			boost::interprocess::open_only, osm_store_filename);
	}

	mmap_file_ptr mmap_file;

	template<typename Func>
	void perform_mmap_operation(Func func) {
		while(true) {
			try {
				func();
				return;
			} catch(boost::interprocess::bad_alloc &e) {
				std::cout << e.what() << std::endl;

				map_size = map_size * 2;
				std::cout << "Resizing osm store to size: " << map_size << std::endl;
				
				mmap_file = nullptr;

				boost::interprocess::managed_mapped_file::grow(osm_store_filename, map_size);
				mmap_file = open_mmap_file();

				nodes.reopen(mmap_file);
				ways.reopen(mmap_file);
			}
		}
	}

public:

	OSMStore() 
	{ 
		mmap_file = create_mmap_file();
		nodes.reopen(mmap_file);
		ways.reopen(mmap_file);
	}

	~OSMStore()
	{
		remove_mmap_file();
	}

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

	WayList<RelationStoreIterator> relations_at(WayID i) const {
		return relations.at(i);
	}

	void relations_insert_front(WayID i, const WayVec &outerWayVec, const WayVec &innerWayVec) {
		relations.insert_front(i, outerWayVec, innerWayVec);
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
		for (auto ot = outers.begin(); ot != outers.end(); ot++) {
			Polygon poly;
			fillPoints(poly.outer(), ot->begin(), ot->end());
			for (auto it = inners.begin(); it != inners.end(); ++it) {
				Ring inner;
				fillPoints(inner, it->begin(), it->end());
				if (geom::within(inner, poly.outer())) { poly.inners().emplace_back(inner); }
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
