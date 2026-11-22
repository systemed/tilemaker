/*! \file */
#ifndef _CLUSTER_H
#define _CLUSTER_H

#include <string>
#include <utility>
#include <vector>
#include "attribute_store.h"

/*
	Point clustering: within a tile, nearby point features that would be written
	with identical attributes are reduced to a single representative point.

	Clustering is done per tile at write time, so a group straddling a tile
	boundary may produce a point either side of it. Isolated points always
	survive as clusters of one.
*/

// Choose a representative for each cluster of points. Points are drawn into
// clusters seeded no more than `distance` apart; each point then joins its
// nearest seed, and the member nearest each cluster's centroid represents it.
// Returns the indices (into `points`) of the representatives, in input order.
std::vector<size_t> clusterPoints(const std::vector<std::pair<int,int>> &points, double distance);

// A key which is equal for two attribute sets exactly when they would be
// written with the same properties at this zoom level
void clusterAttributeKey(const AttributeStore &attributeStore, AttributeIndex attributes, unsigned zoom, std::string &key);

#endif //_CLUSTER_H
