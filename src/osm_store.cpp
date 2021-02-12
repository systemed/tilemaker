
#include "osm_store.h"
#include <iostream>
using namespace std;
namespace bg = boost::geometry;

// Views of data structures.
//

NodeList<NodeVec::const_iterator> makeNodeList(const NodeVec &nodeVec) {
	return { nodeVec.cbegin(), nodeVec.cend() };
}

WayList<WayVec::const_iterator> makeWayList( const WayVec &outerWayVec, const WayVec &innerWayVec) {
	return { outerWayVec.cbegin(), outerWayVec.cend(), innerWayVec.cbegin(), innerWayVec.cend() };
}

//
// Internal data structures.
//

LatpLon NodeStore::at(NodeID i) const {
	try {
		return mLatpLons.first->at(i);
	}
	catch (std::out_of_range &err){
		stringstream ss;
		ss << "Could not find node " << i;
		throw std::out_of_range(ss.str());
	}
}

// @brief Return whether a latp/lon pair is on the store.
// @param i Any possible OSM ID
// @return 1 if found, 0 otherwise
// @note This function is named as count for consistent naming with stl functions.
size_t NodeStore::count(NodeID i) const {
	return mLatpLons.first->count(i);
}

// @brief Insert a latp/lon pair.
// @param i OSM ID of a node
// @param coord a latp/lon pair to be inserted
// @invariant The OSM ID i must be larger than previously inserted OSM IDs of nodes
//            (though unnecessarily for current impl, future impl may impose that)
void NodeStore::insert_back(NodeID i, LatpLon coord) {
	//std::cout << "Insert node: " << i << " " << coord.latp << " " << coord.lon << std::endl;
	while(true) {
		try {
			mLatpLons.first->emplace(i, coord);
			return;

		} catch(std::exception &e) {
			std::cout << e.what() << std::endl;
			map_size = map_size * 2;
			std::cout << "Resizing node store to size: " << map_size << std::endl;
			mLatpLons.second.reset();
			boost::interprocess::managed_mapped_file::grow(osm_store_filename, map_size);
			mLatpLons = open_mmap_file();
		}
	}
}

// @brief Make the store empty
void NodeStore::clear() {
	mLatpLons.first->clear();
}

size_t NodeStore::size() { 
	return mLatpLons.first->size(); 
}


// way store

// @brief Lookup a node list
// @param i OSM ID of a way
// @return A node list
// @exception NotFound
NodeList<WayStoreIterator> WayStore::at(WayID i) const {
	try {
		const auto &way = mNodeLists.at(i);
		return { way.cbegin(), way.cend() };
	}
	catch (std::out_of_range &err){
		stringstream ss;
		ss << "Could not find way " << i;
		throw std::out_of_range(ss.str());
	}
}

bool WayStore::isClosed(WayID i) const {
	const auto &way = mNodeLists.at(i);
	return way.front() == way.back();
}

NodeVec WayStore::nodesFor(WayID i) const {
	return mNodeLists.at(i);
}
NodeID WayStore::firstNode(WayID i) const {
	return mNodeLists.at(i).front();
}
NodeID WayStore::lastNode(WayID i) const {
	return mNodeLists.at(i).back();
}

// @brief Return whether a node list is on the store.
// @param i Any possible OSM ID
// @return 1 if found, 0 otherwise
// @note This function is named as count for consistent naming with stl functions.
size_t WayStore::count(WayID i) const {
	return mNodeLists.count(i);
}

// @brief Insert a node list.
// @param i OSM ID of a way
// @param nodeVec a node vector to be inserted
// @invariant The OSM ID i must be larger than previously inserted OSM IDs of ways
//            (though unnecessarily for current impl, future impl may impose that)
void WayStore::insert_back(WayID i, const NodeVec &nodeVec) {
	mNodeLists.emplace(i, nodeVec);
}

// @brief Make the store empty
void WayStore::clear() {
	mNodeLists.clear();
}

size_t WayStore::size() { return mNodeLists.size(); }


// relation store

// @brief Lookup a way list
// @param i Pseudo OSM ID of a relational way
// @return A way list
// @exception NotFound
WayList<RelationStoreIterator> RelationStore::at(WayID i) const {
	try {
		const auto &outInList = mOutInLists.at(i);
		return { outInList.first.cbegin(), outInList.first.cend(),
			outInList.second.cbegin(), outInList.second.cend() };
	} catch (std::out_of_range &err){
		stringstream ss;
		ss << "Could not find pseudo OSM ID of polygon " << i;
		throw std::out_of_range(ss.str());
	}
}

// @brief Return whether a way list is on the store.
// @param i Any possible OSM ID
// @return 1 if found, 0 otherwise
// @note This function is named as count for consistent naming with stl functions.
size_t RelationStore::count(WayID i) const {
	return mOutInLists.count(i);
}

// @brief Insert a way list.
// @param i Pseudo OSM ID of a relation
// @param outerWayVec A outer way vector to be inserted
// @param innerWayVec A inner way vector to be inserted
// @invariant The pseudo OSM ID i must be smaller than previously inserted pseudo OSM IDs of relations
//            (though unnecessarily for current impl, future impl may impose that)
void RelationStore::insert_front(WayID i, const WayVec &outerWayVec, const WayVec &innerWayVec) {
	mOutInLists.emplace(i, make_pair(outerWayVec, innerWayVec));
}

// @brief Make the store empty
void RelationStore::clear() {
	mOutInLists.clear();
}

size_t RelationStore::size() { return mOutInLists.size(); }


//
// OSM store, containing all above.
//

void OSMStore::reportSize() {
	cout << "Stored " << nodes.size() << " nodes, " << ways.size() << " ways, " << relations.size() << " relations" << endl;
}

void OSMStore::clear() {
	nodes.clear();
	ways.clear();
	relations.clear();
}

// Relation -> MultiPolygon
MultiPolygon OSMStore::wayListMultiPolygon(WayID relId) const {
	return wayListMultiPolygon(relations.at(relId));
}

MultiPolygon OSMStore::wayListMultiPolygon(const WayVec &outerWayVec, const WayVec &innerWayVec) const {
	return wayListMultiPolygon(makeWayList(outerWayVec, innerWayVec));
}

Linestring OSMStore::wayListLinestring(const WayVec &outerWayVec, const WayVec &innerWayVec) const {
	MultiPolygon mp = wayListMultiPolygon(makeWayList(outerWayVec, innerWayVec));
	if(mp.size()>=1) {
		Linestring out;
		for(auto pt: mp[0].outer())
			bg::append(out, pt);
		return out;
	}
	return Linestring();
}

// Way -> Polygon
Polygon OSMStore::nodeListPolygon(WayID wayId) const {
	return nodeListPolygon(ways.at(wayId));
}

Polygon OSMStore::nodeListPolygon(const NodeVec &nodeVec) const {
	return nodeListPolygon(makeNodeList(nodeVec));
}

// Way -> Linestring

Linestring OSMStore::nodeListLinestring(WayID wayId) const {
	return nodeListLinestring(ways.at(wayId));
}

Linestring OSMStore::nodeListLinestring(const NodeVec &nodeVec) const {
	return nodeListLinestring(makeNodeList(nodeVec));
}


