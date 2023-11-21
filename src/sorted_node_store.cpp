#include <iostream>
#include <algorithm>
#include <cstring>
#include <atomic>
#include <map>
#include <bitset>
#include "sorted_node_store.h"
#include "libpopcnt.h"

std::atomic<uint64_t> totalGroups;
std::atomic<uint64_t> totalNodes; // consider leaving this in so we have fast size()
std::atomic<uint64_t> totalGroupSpace;
std::atomic<uint64_t> totalChunks;
std::atomic<uint64_t> chunkSizeFreqs[257];
std::atomic<uint64_t> groupSizeFreqs[257];

// When SortedNodeStore first starts, it's not confident that it has seen an
// entire segment, so it's in "collecting orphans" mode. Once it crosses a
// threshold of 64K elements, it ceases to be in this mode.
//
// Orphans are rounded up across multiple threads, and dealt with in
// the finalize step.
thread_local bool collectingOrphans = true;
thread_local uint64_t groupStart = -1;
thread_local std::vector<NodeStore::element_t>* localNodes = NULL;

SortedNodeStore::SortedNodeStore() {
	// Each group can store 64K nodes. If we allocate 256K slots
	// for groups, we support 2^34 = 17B nodes, or about twice
	// the number used by OSM as of November 2023.
	groups.resize(256 * 1024);
}

void SortedNodeStore::reopen()
{
	for (const auto entry: allocatedMemory)
		void_mmap_allocator::deallocate(entry.first, entry.second);
	allocatedMemory.clear();

	totalNodes = 0;
	totalGroups = 0;
	totalGroupSpace = 0;
	totalChunks = 0;
	memset(chunkSizeFreqs, 0, sizeof(chunkSizeFreqs));
	memset(groupSizeFreqs, 0, sizeof(groupSizeFreqs));
	orphanage.clear();
	workerBuffers.clear();
	groups.clear();
	groups.resize(256 * 1024);
}

SortedNodeStore::~SortedNodeStore() {
	for (const auto entry: allocatedMemory)
		void_mmap_allocator::deallocate(entry.first, entry.second);
}

LatpLon SortedNodeStore::at(const NodeID id) const {
	const size_t groupIndex = id / (GROUP_SIZE * CHUNK_SIZE);
	const size_t chunk = (id % (GROUP_SIZE * CHUNK_SIZE)) / CHUNK_SIZE;
	const uint64_t chunkMaskByte = chunk / 8;
	const uint64_t chunkMaskBit = chunk % 8;

	const uint64_t nodeMaskByte = (id % CHUNK_SIZE) / 8;
	const uint64_t nodeMaskBit = id % 8;

	GroupInfo* groupPtr = groups[groupIndex];

	if (groupPtr == nullptr) {
		std::cerr << "SortedNodeStore::at(" << id << ") uses non-existent group " << groupIndex << std::endl;
		throw std::runtime_error("SortedNodeStore::at bad index");
	}

	size_t chunkOffset = 0;
	{
			chunkOffset = popcnt(groupPtr->chunkMask, chunkMaskByte);
		uint8_t maskByte = groupPtr->chunkMask[chunkMaskByte];
		maskByte = maskByte & ((1 << chunkMaskBit) - 1);
		chunkOffset += popcnt(&maskByte, 1);

		if (!(groupPtr->chunkMask[chunkMaskByte] & (1 << chunkMaskBit)))
			throw std::runtime_error("SortedNodeStore: node missing, no chunk");
	}

	uint16_t scaledOffset = groupPtr->chunkOffsets[chunkOffset];
	ChunkInfo* chunkPtr = (ChunkInfo*)(((char *)(groupPtr->chunkOffsets + popcnt(groupPtr->chunkMask, 32))) + (scaledOffset * 8));

	size_t nodeOffset = 0;
	{
			nodeOffset = popcnt(chunkPtr->nodeMask, nodeMaskByte);
		uint8_t maskByte = chunkPtr->nodeMask[nodeMaskByte];
		maskByte = maskByte & ((1 << nodeMaskBit) - 1);
		nodeOffset += popcnt(&maskByte, 1);
		if (!(chunkPtr->nodeMask[nodeMaskByte] & (1 << nodeMaskBit)))
			throw std::runtime_error("SortedNodeStore: node missing, no node");
}

	return chunkPtr->nodes[nodeOffset];
}

size_t SortedNodeStore::size() const {
	// In general, use our atomic counter - it's fastest.
	return totalNodes.load();

	/*
	// This code can be useful when debugging changes to the internal structure.
	size_t rv = 0;
	size_t totalChunks = 0;
	for (const GroupInfo* group: groups) {
		if (group != nullptr) {
			uint64_t chunks = popcnt(group->chunkMask, 32);
			totalChunks += chunks;

			for (size_t i = 0; i < chunks; i++) {
				size_t rawOffset = group->chunkOffsets[i] * 8;
				ChunkInfo* chunk = (ChunkInfo*)(((char*)(&group->chunkOffsets[chunks])) + rawOffset);
				rv += popcnt(chunk->nodeMask, 32);
			}
		}
	}

	std::cout << "SortedNodeStore::size(): totalChunks=" << totalChunks << ", size=" << rv << " (actual nodes: " << totalNodes.load() << ")" << std::endl;
	return rv;
	*/
}

void SortedNodeStore::insert(const std::vector<element_t>& elements) {
	if (localNodes == NULL) {
		std::lock_guard<std::mutex> lock(orphanageMutex);
		if (workerBuffers.size() == 0)
			workerBuffers.reserve(256);
		else if (workerBuffers.size() == workerBuffers.capacity())
			throw std::runtime_error("SortedNodeStore doesn't support more than 256 cores");
		workerBuffers.push_back(std::vector<element_t>());
		localNodes = &workerBuffers.back();
	}

	if (groupStart == -1) {
		// Mark where the first full group starts, so we know when to transition
		// out of collecting orphans.
		groupStart = elements[0].first / (GROUP_SIZE * CHUNK_SIZE) * (GROUP_SIZE * CHUNK_SIZE);
	}

	int i = 0;
	while (collectingOrphans && i < elements.size()) {
		const element_t& el = elements[i];
		if (el.first >= groupStart + (GROUP_SIZE * CHUNK_SIZE)) {
			collectingOrphans = false;
			// Calculate new groupStart, rounding to previous boundary.
			groupStart = el.first / (GROUP_SIZE * CHUNK_SIZE) * (GROUP_SIZE * CHUNK_SIZE);
			collectOrphans(*localNodes);
			localNodes->clear();
		}
		localNodes->push_back(el);
		i++;
	}

	while(i < elements.size()) {
		const element_t& el = elements[i];

		if (el.first >= groupStart + (GROUP_SIZE * CHUNK_SIZE)) {
			publishGroup(*localNodes);
			localNodes->clear();
			groupStart = el.first / (GROUP_SIZE * CHUNK_SIZE) * (GROUP_SIZE * CHUNK_SIZE);
		}

		localNodes->push_back(el);
		i++;
	}
}

void SortedNodeStore::batchStart() {
	collectingOrphans = true;
	groupStart = -1;
	if (localNodes == NULL || localNodes->size() == 0)
		return;

	collectOrphans(*localNodes);
	localNodes->clear();
}

void SortedNodeStore::finalize(size_t threadNum) {
	for (const auto& buffer: workerBuffers) {
		if (buffer.size() > 0) {
			collectOrphans(buffer);
		}
	}
	workerBuffers.clear();

	// Empty the orphanage into the index.
	std::vector<element_t> copy;
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

	std::cout << "SortedNodeStore: saw " << totalGroups << " groups, " << totalChunks << " chunks, " << totalNodes.load() << " nodes, needed " << totalGroupSpace.load() << " bytes" << std::endl;
	/*
	for (int i = 0; i < 257; i++)
		std::cout << "chunkSizeFreqs[ " << i << " ]= " << chunkSizeFreqs[i].load() << std::endl;
	for (int i = 0; i < 257; i++)
		std::cout << "groupSizeFreqs[ " << i << " ]= " << groupSizeFreqs[i].load() << std::endl;
		*/
}

void SortedNodeStore::collectOrphans(const std::vector<element_t>& orphans) {
	std::lock_guard<std::mutex> lock(orphanageMutex);
	size_t groupIndex = orphans[0].first / (GROUP_SIZE * CHUNK_SIZE);

	std::vector<element_t>& vec = orphanage[groupIndex];
	const size_t i = vec.size();
	vec.resize(i + orphans.size());
	std::copy(orphans.begin(), orphans.end(), vec.begin() + i);
}

void SortedNodeStore::publishGroup(const std::vector<element_t>& nodes) {
	totalNodes += nodes.size();
	if (nodes.size() == 0) {
		throw std::runtime_error("SortedNodeStore: group is empty");
	}
	size_t groupIndex = nodes[0].first / (GROUP_SIZE * CHUNK_SIZE);

	if (nodes.size() > CHUNK_SIZE * GROUP_SIZE) {
		std::cout << "groupIndex=" << groupIndex << ", first ID=" << nodes[0].first << ", nodes.size() = " << nodes.size() << std::endl;
		throw std::runtime_error("SortedNodeStore: group is too big");
	}

	totalGroups++;

	// Calculate the space we need for this group.
	// TODO: the nodes are sorted, so we can calculate # of chunks
	// with a single O(n) pass, don't need a set/map.
	// TODO: until then can use a uint16_t/uint16_t
	std::map<uint64_t, uint64_t> uniqueChunks;
	for (const auto& node: nodes) {
		const size_t chunk = (node.first % (GROUP_SIZE * CHUNK_SIZE)) / CHUNK_SIZE;

		if (uniqueChunks.find(chunk) == uniqueChunks.end())
			uniqueChunks[chunk] = 0;

		uniqueChunks[chunk]++;
	}

	uint64_t chunks = uniqueChunks.size();
	totalChunks += chunks;

	size_t groupSpace =
		sizeof(GroupInfo) + // Every group needs a GroupInfo
		chunks * sizeof(uint16_t); // Offsets for each chunk in GroupInfo

	for (const auto& chunk: uniqueChunks) {
		size_t chunkSpace = 
			sizeof(ChunkInfo) + // Every chunk needs a ChunkInfo
			chunk.second * sizeof(LatpLon); // The actual data

		// We require that chunks align on 8-byte boundaries
		chunkSpace += 8 - (chunkSpace % 8);
		groupSpace += chunkSpace;
	}
	totalGroupSpace += groupSpace;

	// CONSIDER: for small values of `groupSpace`, pull space from a shared pool.
	// e.g. a 1-node Group takes 78 bytes, which is a waste of a malloc
	GroupInfo* groupInfo = nullptr;
	groupInfo = (GroupInfo*)void_mmap_allocator::allocate(groupSpace);
	if (groupInfo == nullptr)
		throw std::runtime_error("failed to allocate space for group");
	{
		std::lock_guard<std::mutex> lock(orphanageMutex);
		allocatedMemory.push_back(std::make_pair((void*)groupInfo, groupSpace));
	}
	if (groups[groupIndex] != nullptr)
		throw std::runtime_error("SortedNodeStore: group already present");
	groups[groupIndex] = groupInfo;

	int64_t lastChunk = -1;
	uint8_t chunkMask[32], nodeMask[32];
	memset(chunkMask, 0, 32);
	memset(nodeMask, 0, 32);

	int64_t currentChunkIndex = 0;
	size_t numNodesInChunk = 0;
	size_t chunkNodeStartIndex = 0;

	char* nextChunkInfo = (char*)&(groupInfo->chunkOffsets[chunks]);

	// NB: `i` goes past the end of `nodes` in order that we have
	//  the chance to publish the final ChunkInfo. We take care
	//  not to read past the end of `nodes`, though.
	for (size_t i = 0; i <= nodes.size(); i++) {
		int64_t currentChunk = -1;

		if (i != nodes.size()) {
			const element_t& node = nodes[i];
			currentChunk = (node.first % (GROUP_SIZE * CHUNK_SIZE)) / CHUNK_SIZE;
		}

		if (currentChunk != lastChunk) {
			if (lastChunk != -1) {
				// Publish a ChunkInfo.

				memcpy(((ChunkInfo*)nextChunkInfo)->nodeMask, nodeMask, 32);

				const size_t rawOffset = nextChunkInfo - (char*)(&groupInfo->chunkOffsets[chunks]);
				const size_t scaledOffset = rawOffset / 8;
				if (rawOffset % 8 != 0)
					throw std::runtime_error("SortedNodeStore: invalid scaledOffset for chunk");
				if (scaledOffset > 65535)
					throw std::runtime_error("SortedNodeStore: scaledOffset too big");

				groupInfo->chunkOffsets[currentChunkIndex] = (uint16_t)(scaledOffset);

				((ChunkInfo*)nextChunkInfo)->size = numNodesInChunk * 8;
				// Copy the actual nodes.
				for (size_t j = chunkNodeStartIndex; j < i; j++) {
					((ChunkInfo*)nextChunkInfo)->nodes[j - chunkNodeStartIndex] = nodes[j].second;
				}

				size_t chunkSpace = 
					sizeof(ChunkInfo) + // Every chunk needs a ChunkInfo
					numNodesInChunk * sizeof(LatpLon); // The actual data

				// We require that chunks align on 8-byte boundaries
				chunkSpace += 8 - (chunkSpace % 8);

				nextChunkInfo += chunkSpace;
				chunkSizeFreqs[numNodesInChunk]++;

				numNodesInChunk = 0;
				memset(nodeMask, 0, 32);

				const uint64_t chunkMaskByte = lastChunk / 8;
				const uint64_t chunkMaskBit = lastChunk % 8;

				chunkMask[chunkMaskByte] |= 1 << chunkMaskBit;
				if (currentChunk != -1)
					currentChunkIndex++;
			}

			lastChunk = currentChunk;
			chunkNodeStartIndex = i;
		}
		numNodesInChunk++;

		if (i != nodes.size()) {
			const element_t& node = nodes[i];

			const uint64_t nodeMaskByte = (node.first % CHUNK_SIZE) / 8;
			const uint64_t nodeMaskBit = node.first % 8;
			nodeMask[nodeMaskByte] |= 1 << nodeMaskBit;
		}
	}

	groupSizeFreqs[currentChunkIndex]++;
	memcpy(groupInfo->chunkMask, chunkMask, 32);
}
