#ifndef _SHARDED_WAY_STORE
#define _SHARDED_WAY_STORE

#include <functional>
#include <memory>
#include "way_store.h"

class NodeStore;

class ShardedWayStore : public WayStore {
public:
	ShardedWayStore(std::function<std::shared_ptr<WayStore>()> createWayStore, const NodeStore& nodeStore);
	~ShardedWayStore();
	void reopen() override;
	void batchStart() override;
	std::vector<LatpLon> at(WayID wayid) const override;
	bool requiresNodes() const override;
	void insertLatpLons(std::vector<WayStore::ll_element_t> &newWays) override;
	void insertNodes(const std::vector<std::pair<WayID, std::vector<NodeID>>>& newWays) override;
	void clear() override;
	std::size_t size() const override;
	void finalize(unsigned int threadNum) override;

	bool contains(size_t shard, WayID id) const override;
	WayStore& shard(size_t shard) override;
	const WayStore& shard(size_t shard) const override;
	size_t shards() const override;
	
private:
	std::function<std::shared_ptr<WayStore>()> createWayStore;
	const NodeStore& nodeStore;
	std::vector<std::shared_ptr<WayStore>> stores;
};

#endif
