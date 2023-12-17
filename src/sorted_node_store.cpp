#include <iostream>
#include <algorithm>
#include <cstring>
#include <string>
#include <map>
#include <bitset>
#include "sorted_node_store.h"
#include "external/libpopcnt.h"
#include "external/streamvbyte.h"
#include "external/streamvbyte_zigzag.h"

namespace SortedNodeStoreTypes {
	const uint16_t GroupSize = 256;
	const uint16_t ChunkSize = 256;
	const uint16_t ChunkAlignment = 16;
	const uint32_t ChunkCompressed = 1 << 31;

	struct ThreadStorage {
		ThreadStorage():
			collectingOrphans(true),
			groupStart(-1),
			localNodes(nullptr),
			cachedChunk(-1),
			arenaSpace(0),
			arenaPtr(nullptr) {}
		// When SortedNodeStore first starts, it's not confident that it has seen an
		// entire segment, so it's in "collecting orphans" mode. Once it crosses a
		// threshold of 64K elements, it ceases to be in this mode.
		//
		// Orphans are rounded up across multiple threads, and dealt with in
		// the finalize step.
		bool collectingOrphans = true;
		uint64_t groupStart = -1;
		std::vector<NodeStore::element_t>* localNodes = nullptr;

		int64_t cachedChunk = -1;
		std::vector<int32_t> cacheChunkLons;
		std::vector<int32_t> cacheChunkLatps;

		uint32_t arenaSpace = 0;
		char* arenaPtr = nullptr;
	};

	thread_local std::deque<std::pair<const SortedNodeStore*, ThreadStorage>> threadStorage;

	ThreadStorage& s(const SortedNodeStore* who) {
		for (auto& entry : threadStorage)
			if (entry.first == who)
				return entry.second;

		threadStorage.push_back(std::make_pair(who, ThreadStorage()));

		auto& rv = threadStorage.back();
		return rv.second;
	}
}

using namespace SortedNodeStoreTypes;

SortedNodeStore::SortedNodeStore(bool compressNodes): compressNodes(compressNodes) {
	reopen();
}

void SortedNodeStore::reopen()
{
	for (const auto entry: allocatedMemory)
		void_mmap_allocator::deallocate(entry.first, entry.second);
	allocatedMemory.clear();

	totalNodes = 0;
	totalGroups = 0;
	totalGroupSpace = 0;
	totalAllocatedSpace = 0;
	totalChunks = 0;
	memset(chunkSizeFreqs, 0, sizeof(chunkSizeFreqs));
	memset(groupSizeFreqs, 0, sizeof(groupSizeFreqs));
	orphanage.clear();
	workerBuffers.clear();

	// Each group can store 64K nodes. If we allocate 256K slots
	// for groups, we support 2^34 = 17B nodes, or about twice
	// the number used by OSM as of November 2023.
	groups.clear();
	groups.resize(256 * 1024);
}

SortedNodeStore::~SortedNodeStore() {
	for (const auto entry: allocatedMemory)
		void_mmap_allocator::deallocate(entry.first, entry.second);
}

LatpLon SortedNodeStore::at(const NodeID id) const {
	const size_t groupIndex = id / (GroupSize * ChunkSize);
	const size_t chunk = (id % (GroupSize * ChunkSize)) / ChunkSize;
	const uint64_t chunkMaskByte = chunk / 8;
	const uint64_t chunkMaskBit = chunk % 8;

	const uint64_t nodeMaskByte = (id % ChunkSize) / 8;
	const uint64_t nodeMaskBit = id % 8;

	GroupInfo* groupPtr = groups[groupIndex];

	if (groupPtr == nullptr) {
		throw std::out_of_range("SortedNodeStore::at(" + std::to_string(id) + ") uses non-existent group " + std::to_string(groupIndex));
	}

	size_t chunkOffset = 0;
	{
		chunkOffset = popcnt(groupPtr->chunkMask, chunkMaskByte);
		uint8_t maskByte = groupPtr->chunkMask[chunkMaskByte];
		maskByte = maskByte & ((1 << chunkMaskBit) - 1);
		chunkOffset += popcnt(&maskByte, 1);

		if (!(groupPtr->chunkMask[chunkMaskByte] & (1 << chunkMaskBit)))
			throw std::out_of_range("SortedNodeStore: node " + std::to_string(id) + " missing, no chunk");
	}

	uint16_t scaledOffset = groupPtr->chunkOffsets[chunkOffset];
	ChunkInfoBase* basePtr = (ChunkInfoBase*)(((char *)(groupPtr->chunkOffsets + popcnt(groupPtr->chunkMask, 32))) + (scaledOffset * ChunkAlignment));

	if (basePtr->flags & ChunkCompressed) {
		CompressedChunkInfo* ptr = (CompressedChunkInfo*)basePtr;
		size_t latpSize = (ptr->flags >> 10) & ((1 << 10) - 1);
		// TODO: we don't actually need the lonSize to decompress the data.
		//       May as well store it as a sanity check for now.
		// size_t lonSize = ptr->flags & ((1 << 10) - 1);
		size_t n = popcnt(ptr->nodeMask, 32) - 1;

		const size_t neededChunk = groupIndex * ChunkSize + chunk;

		// Really naive caching strategy - just cache the last-used chunk.
		// Probably good enough?
		if (s(this).cachedChunk != neededChunk) {
			s(this).cachedChunk = neededChunk;
			s(this).cacheChunkLons.reserve(256);
			s(this).cacheChunkLatps.reserve(256);

			uint8_t* latpData = ptr->data;
			uint8_t* lonData = ptr->data + latpSize;
			uint32_t recovdata[256] = {0};

			streamvbyte_decode(latpData, recovdata, n);
			s(this).cacheChunkLatps[0] = ptr->firstLatp;
			zigzag_delta_decode(recovdata, &s(this).cacheChunkLatps[1], n, s(this).cacheChunkLatps[0]);

			streamvbyte_decode(lonData, recovdata, n);
			s(this).cacheChunkLons[0] = ptr->firstLon;
			zigzag_delta_decode(recovdata, &s(this).cacheChunkLons[1], n, s(this).cacheChunkLons[0]);
		}

		size_t nodeOffset = 0;
		nodeOffset = popcnt(ptr->nodeMask, nodeMaskByte);
		uint8_t maskByte = ptr->nodeMask[nodeMaskByte];
		maskByte = maskByte & ((1 << nodeMaskBit) - 1);
		nodeOffset += popcnt(&maskByte, 1);
		if (!(ptr->nodeMask[nodeMaskByte] & (1 << nodeMaskBit)))
			throw std::out_of_range("SortedNodeStore: node " + std::to_string(id) + " missing, no node");

		return { s(this).cacheChunkLatps[nodeOffset], s(this).cacheChunkLons[nodeOffset] };
	}

	UncompressedChunkInfo* ptr = (UncompressedChunkInfo*)basePtr;
	size_t nodeOffset = 0;
	nodeOffset = popcnt(ptr->nodeMask, nodeMaskByte);
	uint8_t maskByte = ptr->nodeMask[nodeMaskByte];
	maskByte = maskByte & ((1 << nodeMaskBit) - 1);
	nodeOffset += popcnt(&maskByte, 1);
	if (!(ptr->nodeMask[nodeMaskByte] & (1 << nodeMaskBit)))
		throw std::out_of_range("SortedNodeStore: node " + std::to_string(id) + " missing, no node");

	return ptr->nodes[nodeOffset];
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
				size_t rawOffset = group->chunkOffsets[i] * ChunkAlignment;
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
	if (s(this).localNodes == nullptr) {
		std::lock_guard<std::mutex> lock(orphanageMutex);
		if (workerBuffers.size() == 0)
			workerBuffers.reserve(256);
		else if (workerBuffers.size() == workerBuffers.capacity())
			throw std::runtime_error("SortedNodeStore doesn't support more than 256 cores");
		workerBuffers.push_back(std::vector<element_t>());
		s(this).localNodes = &workerBuffers.back();
	}

	if (s(this).groupStart == -1) {
		// Mark where the first full group starts, so we know when to transition
		// out of collecting orphans.
		s(this).groupStart = elements[0].first / (GroupSize * ChunkSize) * (GroupSize * ChunkSize);
	}

	int i = 0;
	while (s(this).collectingOrphans && i < elements.size()) {
		const element_t& el = elements[i];
		if (el.first >= s(this).groupStart + (GroupSize * ChunkSize)) {
			s(this).collectingOrphans = false;
			// Calculate new groupStart, rounding to previous boundary.
			s(this).groupStart = el.first / (GroupSize * ChunkSize) * (GroupSize * ChunkSize);
			collectOrphans(*s(this).localNodes);
			s(this).localNodes->clear();
		}
		s(this).localNodes->push_back(el);
		i++;
	}

	while(i < elements.size()) {
		const element_t& el = elements[i];

		if (el.first >= s(this).groupStart + (GroupSize * ChunkSize)) {
			publishGroup(*s(this).localNodes);
			s(this).localNodes->clear();
			s(this).groupStart = el.first / (GroupSize * ChunkSize) * (GroupSize * ChunkSize);
		}

		s(this).localNodes->push_back(el);
		i++;
	}
}

void SortedNodeStore::batchStart() {
	s(this).collectingOrphans = true;
	s(this).groupStart = -1;
	if (s(this).localNodes == nullptr || s(this).localNodes->size() == 0)
		return;

	collectOrphans(*s(this).localNodes);
	s(this).localNodes->clear();
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

	std::cout << "SortedNodeStore: " << totalGroups << " groups, " << totalChunks << " chunks, " << totalNodes.load() << " nodes, " << totalGroupSpace.load() << " bytes (" << (1000ull * (totalAllocatedSpace.load() - totalGroupSpace.load()) / totalAllocatedSpace.load()) / 10.0 << "% wasted)" << std::endl;
	/*
	for (int i = 0; i < 257; i++)
		std::cout << "chunkSizeFreqs[ " << i << " ]= " << chunkSizeFreqs[i].load() << std::endl;
	for (int i = 0; i < 257; i++)
		std::cout << "groupSizeFreqs[ " << i << " ]= " << groupSizeFreqs[i].load() << std::endl;
		*/
}

void SortedNodeStore::collectOrphans(const std::vector<element_t>& orphans) {
	std::lock_guard<std::mutex> lock(orphanageMutex);
	size_t groupIndex = orphans[0].first / (GroupSize * ChunkSize);

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
	size_t groupIndex = nodes[0].first / (GroupSize * ChunkSize);
	if (groupIndex >= groups.size())
		throw std::runtime_error("SortedNodeStore: unexpected groupIndex " + std::to_string(groupIndex));

	if (nodes.size() > ChunkSize * GroupSize) {
		std::cout << "groupIndex=" << groupIndex << ", first ID=" << nodes[0].first << ", nodes.size() = " << nodes.size() << std::endl;
		throw std::runtime_error("SortedNodeStore: group is too big");
	}

	totalGroups++;

	// Calculate the space we need for this group's chunks.

	// Build up the lat/lons for each chunk; we use this to
	// calculate if a compressed version is more efficient.
	int32_t tmpLatpLons[257 * 2] = {0};
	uint32_t tmpLatpLonsZigzag[257 * 2] = {0};
	// NB that we're storing sparse indexes -- so if we had
	// chunks 3, 6 and 7, only the first 3 indexes (0, 1, 2) would be set.
	// compressed[chunkIndex] = 0 => no chunk, else it's the compressed size
	// (or ~0 to skip compression)
	uint32_t compressedLatpSize[256] = {0};
	uint32_t compressedLonSize[256] = {0};
	int64_t lastChunk = -1;
	int64_t currentChunkIndex = 0;
	int64_t currentNodeIndex = 0;
	uint16_t numberNodesInChunk[256] = {0};
	uint8_t compressedBuffer[256 * 4 * 2];

	for (size_t i = 0; i <= nodes.size(); i++) {
		int64_t currentChunk = -1;

		if (i != nodes.size()) {
			const element_t& node = nodes[i];
			currentChunk = (node.first % (GroupSize * ChunkSize)) / ChunkSize;
		}

		if (lastChunk != currentChunk) {
			if (lastChunk != -1) {
				numberNodesInChunk[currentChunkIndex] = currentNodeIndex;
				compressedLatpSize[currentChunkIndex] = ~0;
				compressedLonSize[currentChunkIndex] = ~0;

				if (compressNodes) {
					// Check to see if compression would help.
					// Zigzag-delta-encode the lats/lons, then compress them.
					tmpLatpLonsZigzag[0] = tmpLatpLons[0];
					tmpLatpLonsZigzag[256] = tmpLatpLons[256];
					zigzag_delta_encode(tmpLatpLons + 1, tmpLatpLonsZigzag + 1, currentNodeIndex - 1, tmpLatpLons[0]);
					zigzag_delta_encode(tmpLatpLons + 256 + 1, tmpLatpLonsZigzag + 256 + 1, currentNodeIndex - 1, tmpLatpLons[256]);

					size_t latsCompressedSize = streamvbyte_encode(tmpLatpLonsZigzag + 1, currentNodeIndex - 1, compressedBuffer);
					size_t lonsCompressedSize = streamvbyte_encode(tmpLatpLonsZigzag + 256 + 1, currentNodeIndex - 1, compressedBuffer);

					size_t uncompressedSize = currentNodeIndex * 8;
					size_t totalCompressedSize =
						latsCompressedSize + lonsCompressedSize + // The compressed buffers
						2 * 4; // The initial delta

					// We only allot 10 bits for storing the size of the compressed array--
					// if we need more than 10 bits, we haven't actually been able to
					// compress the array.
					if (totalCompressedSize < uncompressedSize && latsCompressedSize < 1024 && lonsCompressedSize < 1024) {
						compressedLatpSize[currentChunkIndex] = latsCompressedSize;
						compressedLonSize[currentChunkIndex] = lonsCompressedSize;
					}
				}

				currentChunkIndex++;
				currentNodeIndex = 0;
			}

			lastChunk = currentChunk;
		}

		tmpLatpLons[currentNodeIndex] = nodes[i].second.latp;
		tmpLatpLons[currentNodeIndex + 256] = nodes[i].second.lon;
		currentNodeIndex++;
	}

	uint64_t chunks = currentChunkIndex;
	totalChunks += chunks;

	size_t groupSpace =
		sizeof(GroupInfo) + // Every group needs a GroupInfo
		chunks * sizeof(uint16_t); // Offsets for each chunk in GroupInfo

	for (currentChunkIndex = 0; currentChunkIndex < 256; currentChunkIndex++) {
		if (compressedLatpSize[currentChunkIndex] == 0)
			break;

		size_t chunkSpace = 0;
		if (compressedLatpSize[currentChunkIndex] == ~0) {
			// Store uncompressed.
			chunkSpace = 
				sizeof(UncompressedChunkInfo) +
				numberNodesInChunk[currentChunkIndex] * sizeof(LatpLon);
		} else {
			chunkSpace = 
				sizeof(CompressedChunkInfo) +
				compressedLatpSize[currentChunkIndex] + compressedLonSize[currentChunkIndex];
		}

		// We require that chunks align on 16-byte boundaries
		if (chunkSpace % ChunkAlignment != 0)
			chunkSpace += ChunkAlignment - (chunkSpace % ChunkAlignment);
		groupSpace += chunkSpace;
	}

	// Per https://github.com/lemire/streamvbyte:
	// During decoding, the library may read up to STREAMVBYTE_PADDING extra
	// bytes from the input buffer (these bytes are read but never used).
	//
	// Thus, we need to reserve at least that much extra to ensure we don't
	// have an out-of-bounds access. We could also allocate from an arena
	// to amortize the cost across many groups, but with 256K groups,
	// the overhead is only 4M, so who cares.
	groupSpace += STREAMVBYTE_PADDING;
	totalGroupSpace += groupSpace;

	GroupInfo* groupInfo = nullptr;

	if (s(this).arenaSpace < groupSpace) {
		// A full group takes ~330KB. Nodes are read _fast_, and there ends
		// up being contention calling the allocator when reading the
		// planet on a machine with 48 cores -- so allocate in large chunks.
		s(this).arenaSpace = 4 * 1024 * 1024;
		totalAllocatedSpace += s(this).arenaSpace;
		s(this).arenaPtr = (char*)void_mmap_allocator::allocate(s(this).arenaSpace);
		if (s(this).arenaPtr == nullptr)
			throw std::runtime_error("SortedNodeStore: failed to allocate arena");
		std::lock_guard<std::mutex> lock(orphanageMutex);
		allocatedMemory.push_back(std::make_pair((void*)s(this).arenaPtr, s(this).arenaSpace));
	}

	s(this).arenaSpace -= groupSpace;
	groupInfo = (GroupInfo*)s(this).arenaPtr;
	s(this).arenaPtr += groupSpace;

	if (groups[groupIndex] != nullptr)
		throw std::runtime_error("SortedNodeStore: group already present");
	groups[groupIndex] = groupInfo;

	lastChunk = -1;
	uint8_t chunkMask[32], nodeMask[32];
	memset(chunkMask, 0, 32);
	memset(nodeMask, 0, 32);

	currentChunkIndex = 0;
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
			currentChunk = (node.first % (GroupSize * ChunkSize)) / ChunkSize;
		}

		if (currentChunk != lastChunk) {
			if (lastChunk != -1) {
				// Publish a ChunkInfo.

				const size_t rawOffset = nextChunkInfo - (char*)(&groupInfo->chunkOffsets[chunks]);
				const size_t scaledOffset = rawOffset / ChunkAlignment;
				if (rawOffset % ChunkAlignment != 0)
					throw std::runtime_error("SortedNodeStore: invalid scaledOffset for chunk");
				if (scaledOffset > 65535)
					throw std::runtime_error("SortedNodeStore: scaledOffset too big (" + std::to_string(scaledOffset) + "), groupIndex=" + std::to_string(groupIndex));

				groupInfo->chunkOffsets[currentChunkIndex] = (uint16_t)(scaledOffset);

				memcpy(((ChunkInfoBase*)nextChunkInfo)->nodeMask, nodeMask, 32);
				if (compressedLatpSize[currentChunkIndex] == ~0) {
					// Store uncompressed.
					((ChunkInfoBase*)nextChunkInfo)->flags = 0;
					for (size_t j = chunkNodeStartIndex; j < i; j++) {
						UncompressedChunkInfo* ptr = (UncompressedChunkInfo*)nextChunkInfo;
						ptr->nodes[j - chunkNodeStartIndex] = nodes[j].second;
					}
				} else {
					// Store compressed.
					CompressedChunkInfo* ptr = (CompressedChunkInfo*)nextChunkInfo;
					ptr->flags = ChunkCompressed | (compressedLatpSize[currentChunkIndex] << 10) | compressedLonSize[currentChunkIndex];

					ptr->firstLatp = nodes[chunkNodeStartIndex].second.latp;
					ptr->firstLon = nodes[chunkNodeStartIndex].second.lon;
					for (size_t j = chunkNodeStartIndex; j < i; j++) {
						tmpLatpLons[j - chunkNodeStartIndex] = nodes[j].second.latp;
						tmpLatpLons[j - chunkNodeStartIndex + 256] = nodes[j].second.lon;
					}

					tmpLatpLonsZigzag[0] = tmpLatpLons[0];
					tmpLatpLonsZigzag[256] = tmpLatpLons[256];
					currentNodeIndex = i - chunkNodeStartIndex;
					zigzag_delta_encode(tmpLatpLons + 1, tmpLatpLonsZigzag + 1, currentNodeIndex - 1, tmpLatpLons[0]);
					zigzag_delta_encode(tmpLatpLons + 256 + 1, tmpLatpLonsZigzag + 256 + 1, currentNodeIndex - 1, tmpLatpLons[256]);

					size_t latsCompressedSize = streamvbyte_encode(tmpLatpLonsZigzag + 1, currentNodeIndex - 1, ptr->data);

					if (latsCompressedSize != compressedLatpSize[currentChunkIndex])
						throw std::runtime_error("unexpected latsCompressedSize");
					size_t lonsCompressedSize = streamvbyte_encode(tmpLatpLonsZigzag + 256 + 1, currentNodeIndex - 1, ptr->data + latsCompressedSize);
					if (lonsCompressedSize != compressedLonSize[currentChunkIndex])
						throw std::runtime_error("unexpected lonsCompressedSize");
				}

				size_t chunkSpace = 0;
				if (compressedLatpSize[currentChunkIndex] == ~0) {
					// Store uncompressed.
					chunkSpace = 
						sizeof(UncompressedChunkInfo) +
						numberNodesInChunk[currentChunkIndex] * sizeof(LatpLon);
				} else {
					chunkSpace = 
						sizeof(CompressedChunkInfo) +
						compressedLatpSize[currentChunkIndex] + compressedLonSize[currentChunkIndex];
				}

				// We require that chunks align on 16-byte boundaries
				if (chunkSpace % ChunkAlignment != 0)
					chunkSpace += ChunkAlignment - (chunkSpace % ChunkAlignment);

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

			const uint64_t nodeMaskByte = (node.first % ChunkSize) / 8;
			const uint64_t nodeMaskBit = node.first % 8;
			nodeMask[nodeMaskByte] |= 1 << nodeMaskBit;
		}
	}

	groupSizeFreqs[currentChunkIndex]++;
	memcpy(groupInfo->chunkMask, chunkMask, 32);

	/*
	// debug: verify that we can read every node we just wrote
	for (const auto& node: nodes) {
		const auto rv = at(node.first);

		if (rv.latp != node.second.latp || rv.lon != node.second.lon)
			throw std::runtime_error("failed to roundtrip node ID " + std::to_string(node.first));
	}
	*/
}
