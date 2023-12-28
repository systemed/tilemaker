#include <algorithm>
#include <boost/sort/sort.hpp>
#include "node_stores.h"

void BinarySearchNodeStore::reopen()
{
	std::lock_guard<std::mutex> lock(mutex);
	for (auto i = 0; i < mLatpLons.size(); i++)
		mLatpLons[i]->clear();

	mLatpLons.clear();
	for (auto i = 0; i < NODE_SHARDS; i++) {
		mLatpLons.push_back(std::make_unique<map_t>());
	}
}

bool BinarySearchNodeStore::contains(size_t shard, NodeID i) const {
	auto internalShard = mLatpLons[shardPart(i)];
	auto id = idPart(i);

	auto iter = std::lower_bound(internalShard->begin(), internalShard->end(), id, [](auto const &e, auto i) { 
		return e.first < i; 
	});

	return !(iter == internalShard->end() || iter->first != id);
}

LatpLon BinarySearchNodeStore::at(NodeID i) const {
	auto shard = mLatpLons[shardPart(i)];
	auto id = idPart(i);

	auto iter = std::lower_bound(shard->begin(), shard->end(), id, [](auto const &e, auto i) { 
		return e.first < i; 
	});

	if(iter == shard->end() || iter->first != id)
		throw std::out_of_range("Could not find node with id " + std::to_string(i));

	return iter->second;
}

size_t BinarySearchNodeStore::size() const {
	std::lock_guard<std::mutex> lock(mutex);
	uint64_t size = 0;
	for (auto i = 0; i < mLatpLons.size(); i++)
		size += mLatpLons[i]->size(); 

	return size;
}

void BinarySearchNodeStore::insert(const std::vector<element_t>& elements) {
	uint32_t newEntries[NODE_SHARDS] = {};
	std::vector<map_t::iterator> iterators;

	// Before taking the lock, do a pass to find out how much
	// to grow each backing collection
	for (auto it = elements.begin(); it != elements.end(); it++) {
		newEntries[shardPart(it->first)]++;
	}

	std::lock_guard<std::mutex> lock(mutex);
	for (auto i = 0; i < NODE_SHARDS; i++) {
		auto size = mLatpLons[i]->size();
		mLatpLons[i]->resize(size + newEntries[i]);
		iterators.push_back(mLatpLons[i]->begin() + size);
	}

	for (auto it = elements.begin(); it != elements.end(); it++) {
		uint32_t shard = shardPart(it->first);
		uint32_t id = idPart(it->first);

		*iterators[shard] = std::make_pair(id, it->second);
		iterators[shard]++;
	}
}

void BinarySearchNodeStore::finalize(size_t threadNum) {
	std::lock_guard<std::mutex> lock(mutex);
	for (auto i = 0; i < NODE_SHARDS; i++) {
		boost::sort::block_indirect_sort(
			mLatpLons[i]->begin(), mLatpLons[i]->end(), 
			[](auto const &a, auto const &b) { return a.first < b.first; },
			threadNum);
	}
}

void CompactNodeStore::reopen()
{
	std::lock_guard<std::mutex> lock(mutex);
	mLatpLons = std::make_unique<map_t>();
}

LatpLon CompactNodeStore::at(NodeID i) const {
	if(i >= mLatpLons->size())
		throw std::out_of_range("Could not find node with id " + std::to_string(i));
	return mLatpLons->at(i);
}

size_t CompactNodeStore::size() const { 
	std::lock_guard<std::mutex> lock(mutex);
	return mLatpLons->size(); 
}

void CompactNodeStore::insert(const std::vector<element_t>& elements) {
	std::lock_guard<std::mutex> lock(mutex);
	for(auto const &i: elements) 
		insert_back(i.first, i.second);
}

// @brief Make the store empty
void CompactNodeStore::clear() { 
	std::lock_guard<std::mutex> lock(mutex);
	mLatpLons->clear(); 
}


