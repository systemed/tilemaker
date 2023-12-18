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
	void clear() { 
		reopen();
	}

	bool contains(size_t shard, NodeID id) const override;
	size_t shards() const override;

private:
	std::function<std::shared_ptr<NodeStore>()> createNodeStore;
	std::vector<std::shared_ptr<NodeStore>> stores;
};

#endif
