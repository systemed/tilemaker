#include "sharded_way_store.h"
#include "node_store.h"

thread_local size_t lastWayShard = 0;

ShardedWayStore::ShardedWayStore(std::function<std::shared_ptr<WayStore>()> createWayStore, const NodeStore& nodeStore):
	createWayStore(createWayStore),
	nodeStore(nodeStore) {
	for (int i = 0; i < shards(); i++)
		stores.push_back(createWayStore());
}

ShardedWayStore::~ShardedWayStore() {
}

void ShardedWayStore::reopen() {
	for (auto& store : stores)
		store->reopen();
}

void ShardedWayStore::batchStart() {
	for (auto& store : stores)
		store->batchStart();
}

std::vector<LatpLon> ShardedWayStore::at(WayID wayid) const {
	for (int i = 0; i < shards(); i++) {
		size_t index = (lastWayShard + i) % shards();
		if (stores[index]->contains(0, wayid)) {
			lastWayShard = index;
			return stores[index]->at(wayid);
		}
	}

	// Superfluous return to silence a compiler warning
	return stores[shards() - 1]->at(wayid);
}

bool ShardedWayStore::requiresNodes() const {
	return stores[0]->requiresNodes();
}

void ShardedWayStore::insertLatpLons(std::vector<WayStore::ll_element_t> &newWays) {
	throw std::runtime_error("ShardedWayStore::insertLatpLons: don't call this directly");
}

void ShardedWayStore::insertNodes(const std::vector<std::pair<WayID, std::vector<NodeID>>>& newWays) {
	throw std::runtime_error("ShardedWayStore::insertNodes: don't call this directly");
}

void ShardedWayStore::clear() {
	for (auto& store : stores)
		store->clear();
}

std::size_t ShardedWayStore::size() const {
	size_t rv = 0;
	for (auto& store : stores)
		rv += store->size();
	return rv;
}

void ShardedWayStore::finalize(unsigned int threadNum) {
	for (auto& store : stores)
		store->finalize(threadNum);
}

bool ShardedWayStore::contains(size_t shard, WayID id) const {
	return stores[shard]->contains(0, id);
}

WayStore& ShardedWayStore::shard(size_t shard) {
	return *stores[shard].get();
}

size_t ShardedWayStore::shards() const { return nodeStore.shards(); }

