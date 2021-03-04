
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


// relation store

// @brief Lookup a way list
// @param i Pseudo OSM ID of a relational way
// @return A way list
// @exception NotFound
WayList<RelationStore::const_iterator> RelationStore::at(WayID i) const {
	try {
		const auto &outInList = mOutInLists->at(i);
		return { outInList.first.cbegin(), outInList.first.cend(), outInList.second.cbegin(), outInList.second.cend() };
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
	return mOutInLists->count(i);
}

// @brief Make the store empty
void RelationStore::clear() {
	mOutInLists->clear();
}

size_t RelationStore::size() const { 
	return mOutInLists->size(); 
}


