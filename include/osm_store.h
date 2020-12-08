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

//
// Views of data structures.
//

template<class NodeIt>
struct NodeList {
	NodeIt begin;
	NodeIt end;
};

NodeList<NodeVec::const_iterator> makeNodeList(const NodeVec &nodeVec);

template<class WayIt>
struct WayList {
	WayIt outerBegin;
	WayIt outerEnd;
	WayIt innerBegin;
	WayIt innerEnd;
};

WayList<WayVec::const_iterator> makeWayList( const WayVec &outerWayVec, const WayVec &innerWayVec);

//
// Internal data structures.
//

// node store
class NodeStore {
	tsl::sparse_map<NodeID, LatpLon> mLatpLons;

public:
	// @brief Lookup a latp/lon pair
	// @param i OSM ID of a node
	// @return Latp/lon pair
	// @exception NotFound
	LatpLon at(NodeID i) const;

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
typedef std::vector<NodeID>::const_iterator WayStoreIterator;

class WayStore {
	tsl::sparse_map<WayID, const std::vector<NodeID> > mNodeLists;

public:
	// @brief Lookup a node list
	// @param i OSM ID of a way
	// @return A node list
	// @exception NotFound
	NodeList<WayStoreIterator> at(WayID i) const;

	bool isClosed(WayID i) const;
	NodeVec nodesFor(WayID i) const;
	NodeID firstNode(WayID i) const;
	NodeID lastNode(WayID i) const;

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
	void insert_back(WayID i, const NodeVec &nodeVec);

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

public:
	NodeStore nodes;
	WayStore ways;
	RelationStore relations;

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
			fillPoints(poly.outer(), *ot);
			for (auto it = inners.begin(); it != inners.end(); ++it) {
				Ring inner;
				fillPoints(inner, *it);
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
				if (ways.isClosed(*it)) {
					// if start==end, simply add it to the set
					results.emplace_back(ways.nodesFor(*it));
					added++;
					done[*it] = true;
				} else {
					// otherwise, can we find a matching outer to append it to?
					bool joined = false;
					NodeVec nodes = ways.nodesFor(*it);
					NodeID jFirst = nodes.front();
					NodeID jLast  = nodes.back();
					for (auto ot = results.begin(); ot != results.end(); ot++) {
						NodeID oFirst = ot->front();
						NodeID oLast  = ot->back();
						if (jFirst==jLast) continue; // don't join to already-closed ways
						else if (oLast==jFirst) {
							// append to the original
							ot->insert(ot->end(), nodes.begin(), nodes.end());
							joined=true; break;
						} else if (oLast==jLast) {
							// append reversed to the original
							ot->insert(ot->end(), nodes.rbegin(), nodes.rend());
							joined=true; break;
						} else if (jLast==oFirst) {
							// prepend to the original
							ot->insert(ot->begin(), nodes.begin(), nodes.end());
							joined=true; break;
						} else if (jFirst==oFirst) {
							// prepend reversed to the original
							ot->insert(ot->begin(), nodes.rbegin(), nodes.rend());
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
					results.emplace_back(ways.nodesFor(*it));
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
		fillPoints(poly.outer(), nodeList);
		geom::correct(poly);
		return poly;
	}

	Polygon nodeListPolygon(WayID wayId) const;

	Polygon nodeListPolygon(const NodeVec &nodeVec) const;

	// Way -> Linestring
	template<class NodeIt>
	Linestring nodeListLinestring(NodeList<NodeIt> nodeList) const{
		Linestring ls;
		fillPoints(ls, nodeList);
		return ls;
	}

	Linestring nodeListLinestring(WayID wayId) const;

	Linestring nodeListLinestring(const NodeVec &nodeVec) const;

private:
	// helper
	template<class PointRange, class NodeIt>
	void fillPoints(PointRange &points, NodeList<NodeIt> nodeList) const {
		for (auto it = nodeList.begin; it != nodeList.end; ++it) {
			LatpLon ll = nodes.at(*it);
			geom::range::push_back(points, geom::make<Point>(ll.lon/10000000.0, ll.latp/10000000.0));
		}
	}

	// fixme - this is duplicate code from above
	template<class PointRange>
	void fillPoints(PointRange &points, NodeVec nodeList) const {
		for (auto it = nodeList.begin(); it != nodeList.end(); ++it) {
			LatpLon ll = nodes.at(*it);
			geom::range::push_back(points, geom::make<Point>(ll.lon/10000000.0, ll.latp/10000000.0));
		}
	}
};

#endif //_OSM_STORE_H
