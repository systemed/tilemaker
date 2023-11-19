#ifndef _NODE_STORE_H
#define _NODE_STORE_H

#include "coordinates.h"
#include <utility>

class NodeStore
{
public:
	using element_t = std::pair<NodeID, LatpLon>;

	// Mutators
	virtual void insert(const std::vector<element_t>& elements) = 0;
	virtual void finalize(size_t threadNum) = 0;
	virtual void clear() = 0;
	virtual void reopen() = 0;

	// Accessors
	virtual size_t size() const = 0;
	virtual LatpLon at(NodeID i) const = 0;
};

#endif
