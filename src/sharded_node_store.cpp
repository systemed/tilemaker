#include "sharded_node_store.h"

thread_local size_t lastNodeShard = 0;

ShardedNodeStore::ShardedNodeStore(std::function<std::shared_ptr<NodeStore>()> createNodeStore):
	createNodeStore(createNodeStore) {
	for (int i = 0; i < shards(); i++)
		stores.push_back(createNodeStore());
}

ShardedNodeStore::~ShardedNodeStore() {
}

void ShardedNodeStore::reopen() {
	for (auto& store : stores)
		store->reopen();
}

void ShardedNodeStore::finalize(size_t threadNum) {
	for (auto& store : stores)
		store->finalize(threadNum);
}

LatpLon ShardedNodeStore::at(NodeID id) const {
	for (int i = 0; i < shards(); i++) {
		size_t index = (lastNodeShard + i) % shards();

		if (stores[index]->contains(0, id)) {
			lastNodeShard = index;
			return stores[index]->at(id);
		}
	}

	// Superfluous return to silence a compiler warning
	return stores[shards() - 1]->at(id);
}

size_t ShardedNodeStore::size() const {
	size_t rv = 0;
	for (auto& store : stores)
		rv += store->size();

	return rv;
}

void ShardedNodeStore::batchStart() {
	for (auto& store : stores)
		store->batchStart();
}

size_t pickStore(const LatpLon& el) {
	// Assign the element to a store. This is pretty naive, we could likely do better--
	// Europe still basically gets its own bucket, but probably should be split up
	// more.

	const size_t z3x = lon2tilex(el.lon / 10000000, 3);
	const size_t z3y = latp2tiley(el.latp / 10000000, 3);

	if (z3x == 4 && z3y == 2) return 4; // Central Europe
	if (z3x == 5 && z3y == 2) return 5; // Western Russia
	if (z3x == 4 && z3y == 3) return 6; // North Africa
	if (z3x == 5 && z3y == 3) return 7; // India

	const size_t z2x = z3x / 2;
	const size_t z2y = z3y / 2;

	if (z2x == 3 && z2y == 1) return 3; // Asia, Russia
	if (z2x == 1 && z2y == 1) return 2; // North Atlantic Ocean and bordering countries
	if (z2x == 0 && z2y == 1) return 1; // North America

//	std::cout << "z2x=" << std::to_string(z2x) << ", z2y=" << std::to_string(z2y) << std::endl;
	return 0; // Artic, Antartcica, Oceania
}

void ShardedNodeStore::insert(const std::vector<element_t>& elements) {
	std::vector<std::vector<element_t>> perStore(shards());

	for (const auto& el : elements) {
		perStore[pickStore(el.second)].push_back(el);
	}

	for (int i = 0; i < shards(); i++) {
		if (!perStore[i].empty())
			stores[i]->insert(perStore[i]);
	}
}

bool ShardedNodeStore::contains(size_t shard, NodeID id) const {
	return stores[shard]->contains(0, id);
}

size_t ShardedNodeStore::shards() const {
	return 8;
}
