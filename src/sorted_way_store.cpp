#include <atomic>
#include <algorithm>
#include <iostream>
#include "sorted_way_store.h"

namespace SortedWayStoreTypes {
	const uint16_t GroupSize = 256;
	const uint16_t ChunkSize = 256;
	thread_local bool collectingOrphans = true;
	thread_local uint64_t groupStart = -1;
	thread_local std::vector<std::pair<WayID, std::vector<NodeID>>>* localWays = NULL;

	std::atomic<uint64_t> totalWays;
	std::atomic<uint64_t> totalGroups;
	std::atomic<uint64_t> totalGroupSpace;
	std::atomic<uint64_t> totalChunks;
}

using namespace SortedWayStoreTypes;


void SortedWayStore::reopen() {
	std::cout << "TODO: SortedWayStore::reopen()" << std::endl;
}

std::vector<LatpLon> SortedWayStore::at(WayID wayid) const {
	throw std::runtime_error("at() notimpl");
}

void SortedWayStore::insertLatpLons(std::vector<WayStore::ll_element_t> &newWays) {
	throw std::runtime_error("SortedWayStore does not support insertLatpLons");
}

const void SortedWayStore::insertNodes(const std::vector<std::pair<WayID, std::vector<NodeID>>>& newWays) {
	if (localWays == nullptr) {
		std::lock_guard<std::mutex> lock(orphanageMutex);
		if (workerBuffers.size() == 0)
			workerBuffers.reserve(256);
		else if (workerBuffers.size() == workerBuffers.capacity())
			throw std::runtime_error("SortedWayStore doesn't support more than 256 cores");
		workerBuffers.push_back(std::vector<std::pair<WayID, std::vector<NodeID>>>());
		localWays = &workerBuffers.back();
	}

	if (groupStart == -1) {
		// Mark where the first full group starts, so we know when to transition
		// out of collecting orphans.
		groupStart = newWays[0].first / (GroupSize * ChunkSize) * (GroupSize * ChunkSize);
	}

	int i = 0;
	while (collectingOrphans && i < newWays.size()) {
		const auto& el = newWays[i];
		if (el.first >= groupStart + (GroupSize * ChunkSize)) {
			collectingOrphans = false;
			// Calculate new groupStart, rounding to previous boundary.
			groupStart = el.first / (GroupSize * ChunkSize) * (GroupSize * ChunkSize);
			collectOrphans(*localWays);
			localWays->clear();
		}
		localWays->push_back(el);
		i++;
	}

	while(i < newWays.size()) {
		const auto& el = newWays[i];

		if (el.first >= groupStart + (GroupSize * ChunkSize)) {
			publishGroup(*localWays);
			localWays->clear();
			groupStart = el.first / (GroupSize * ChunkSize) * (GroupSize * ChunkSize);
		}

		localWays->push_back(el);
		i++;
	}
}

void SortedWayStore::clear() {
	std::cout << "TODO: SortedWayStore::clear()" << std::endl;
}

std::size_t SortedWayStore::size() const {
	throw std::runtime_error("size() notimpl");
}

void SortedWayStore::finalize(unsigned int threadNum) {
	for (const auto& buffer: workerBuffers) {
		if (buffer.size() > 0) {
			collectOrphans(buffer);
		}
	}
	workerBuffers.clear();

	// Empty the orphanage into the index.
	std::vector<std::pair<WayID, std::vector<NodeID>>> copy;
	for (const auto& entry: orphanage) {
		for (const auto& orphan: entry.second)
			copy.push_back(orphan);

		// Orphans may come from different workers, and thus be unsorted.
		std::sort(
			copy.begin(),
			copy.end(), 
			[](auto const &a, auto const &b) { return a.first < b.first; }
		);
		publishGroup(copy);
		copy.clear();
	}

	orphanage.clear();

	std::cout << "SortedWayStore: " << totalGroups << " groups, " << totalChunks << " chunks, " << totalWays.load() << " ways, " << totalGroupSpace.load() << " bytes" << std::endl;
	/*
	for (int i = 0; i < 257; i++)
		std::cout << "chunkSizeFreqs[ " << i << " ]= " << chunkSizeFreqs[i].load() << std::endl;
	for (int i = 0; i < 257; i++)
		std::cout << "groupSizeFreqs[ " << i << " ]= " << groupSizeFreqs[i].load() << std::endl;
		*/

}

void SortedWayStore::batchStart() {
	collectingOrphans = true;
	groupStart = -1;
	if (localWays == nullptr || localWays->size() == 0)
		return;

	collectOrphans(*localWays);
	localWays->clear();
}

void SortedWayStore::collectOrphans(const std::vector<std::pair<WayID, std::vector<NodeID>>>& orphans) {
	std::lock_guard<std::mutex> lock(orphanageMutex);
	size_t groupIndex = orphans[0].first / (GroupSize * ChunkSize);

	std::vector<std::pair<WayID, std::vector<NodeID>>>& vec = orphanage[groupIndex];
	const size_t i = vec.size();
	vec.resize(i + orphans.size());
	std::copy(orphans.begin(), orphans.end(), vec.begin() + i);
}

void SortedWayStore::publishGroup(const std::vector<std::pair<WayID, std::vector<NodeID>>>& ways) {
	// Start with a very naive approach, store the entire thing.
	totalWays += ways.size();
	if (ways.size() == 0) {
		throw std::runtime_error("SortedWayStore: group is empty");
	}
	size_t groupIndex = ways[0].first / (GroupSize * ChunkSize);

	if (ways.size() > ChunkSize * GroupSize) {
		std::cout << "groupIndex=" << groupIndex << ", first ID=" << ways[0].first << ", ways.size() = " << ways.size() << std::endl;
		throw std::runtime_error("SortedWayStore: group is too big");
	}

	totalGroups++;

}


