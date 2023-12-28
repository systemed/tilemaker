#ifndef _SHARDED_NODE_STORE
#define _SHARDED_NODE_STORE

#include <functional>
#include <memory>
#include "node_store.h"

class ShardedNodeStore : public NodeStore {
public:
	ShardedNodeStore(std::function<std::shared_ptr<NodeStore>()> createNodeStore);
	~ShardedNodeStore();
	void reopen() override;
	void finalize(size_t threadNum) override;
	LatpLon at(NodeID i) const override;
	size_t size() const override;
	void batchStart() override;
	void insert(const std::vector<element_t>& elements) override;
	void clear() override {
		reopen();
	}

	bool contains(size_t shard, NodeID id) const override;
	NodeStore& shard(size_t shard) override { return *stores[shard]; }
	const NodeStore& shard(size_t shard) const override { return *stores[shard]; }
	size_t shards() const override;

private:
	std::function<std::shared_ptr<NodeStore>()> createNodeStore;
	std::vector<std::shared_ptr<NodeStore>> stores;
};

#endif
