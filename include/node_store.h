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
	virtual void clear() = 0;
	virtual void reopen() = 0;
	// Run on each thread when a batch of blocks is started. Only
	// meaningful for SortedNodeStore
	virtual void batchStart() = 0;

	// Run on a single-thread, after all nodes have been inserted.
	virtual void finalize(size_t threadNum) = 0;

	// Accessors
	virtual size_t size() const = 0;
	virtual LatpLon at(NodeID i) const = 0;
};

#endif
