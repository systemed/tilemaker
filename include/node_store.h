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
	// Batched lookup: out[i] = at(ids[i]) for i < n. Throws exactly what
	// at() throws for the first missing node.
	virtual void at(const NodeID* ids, size_t n, LatpLon* out) const {
		for (size_t i = 0; i < n; i++)
			out[i] = at(ids[i]);
	}

	virtual bool contains(size_t shard, NodeID id) const = 0;
	virtual NodeStore& shard(size_t shard) = 0;
	virtual const NodeStore& shard(size_t shard) const = 0;
	virtual size_t shards() const = 0;
};

#endif
