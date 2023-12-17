#ifndef _NODE_STORES_H
#define _NODE_STORES_H

#include <mutex>
#include <memory>
#include "node_store.h"
#include "sorted_node_store.h"
#include "mmap_allocator.h"

class BinarySearchNodeStore : public NodeStore
{

public:
	using internal_element_t = std::pair<ShardedNodeID, LatpLon>;
	using map_t = std::deque<internal_element_t, mmap_allocator<internal_element_t>>;

	void reopen() override;
	void finalize(size_t threadNum) override;
	LatpLon at(NodeID i) const override;
	size_t size() const override;
	void insert(const std::vector<element_t>& elements) override;
	void clear() { 
		reopen();
	}
	void batchStart() {}

	bool contains(size_t shard, NodeID id) const override;
	size_t shard() const override { return 0; }
	size_t shards() const override { return 1; }
	

private: 
	mutable std::mutex mutex;
	std::vector<std::shared_ptr<map_t>> mLatpLons;

	uint32_t shardPart(NodeID id) const {
		uint32_t rv = id >> 32;
		return rv;
	}

	uint32_t idPart(NodeID id) const { return id; }
};

class CompactNodeStore : public NodeStore
{

public:
	using element_t = std::pair<NodeID, LatpLon>;
	using map_t = std::deque<LatpLon, mmap_allocator<LatpLon>>;

	void reopen() override;
	LatpLon at(NodeID i) const override;
	size_t size() const override;
	void insert(const std::vector<element_t>& elements) override;
	void clear() override;
	void finalize(size_t numThreads) override {}
	void batchStart() {}

	// CompactNodeStore has no metadata to know whether or not it contains
	// a node, so it's not suitable for used in sharded scenarios.
	bool contains(size_t shard, NodeID id) const override { return true; }
	size_t shard() const override { return 0; }
	size_t shards() const override { return 1; }

private: 
	// @brief Insert a latp/lon pair.
	// @param i OSM ID of a node
	// @param coord a latp/lon pair to be inserted
	// @invariant The OSM ID i must be larger than previously inserted OSM IDs of nodes
	//			  (though unnecessarily for current impl, future impl may impose that)
	void insert_back(NodeID i, LatpLon coord) {
		if(i >= mLatpLons->size())
			mLatpLons->resize(i + 1);
		(*mLatpLons)[i] = coord;
	}


	mutable std::mutex mutex;
	std::shared_ptr<map_t> mLatpLons;
};


#endif
