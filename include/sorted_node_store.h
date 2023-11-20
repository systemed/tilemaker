#ifndef _SORTED_NODE_STORE_H
#define _SORTED_NODE_STORE_H

#include "node_store.h"
#include "mmap_allocator.h"
#include <map>
#include <memory>
#include <mutex>

// SortedNodeStore requires the Sort.Type_then_ID property on the source PBF.
//
// It stores nodes in chunks of 256, and chunks in groups of 256.
// Access to a node given its NodeID is constant time.
//
// Additional memory usage varies, approaching 1% for very large PBFs.
#define GROUP_SIZE 256
#define CHUNK_SIZE 256

struct ChunkInfo {
	// A bitmask indicating how many nodes are in this chunk.
	uint8_t nodeMask[32];
	// The length of the data, starting at &nodes[0].
	// 32 bits is way more than we need, we may eventually use the
	// upper 2 bytes for flags, e.g. for tracking compression.
	uint32_t size;
	LatpLon nodes[0];
};

struct GroupInfo {
	// A bitmask indicating how many chunks are in this group.
	uint8_t chunkMask[32];

	// There is an entry for each set bit in chunkMask. They identify
	// the address of a ChunkInfo. The address is relative to the end
	// of the GroupInfo struct
	//
	// e.g. given an offset 12, the chunk is located at
	// &chunkOffsets[popcnt(chunkMask)] + offset * 8.
	uint16_t chunkOffsets[0];
};


class SortedNodeStore : public NodeStore
{

public:
	using internal_element_t = std::pair<ShardedNodeID, LatpLon>;
	using map_t = std::deque<internal_element_t, mmap_allocator<internal_element_t>>;

	SortedNodeStore();
	void reopen() override;
	void finalize(size_t threadNum) override;
	LatpLon at(NodeID i) const override;
	size_t size() const override;
	void batchStart() override;
	void insert(const std::vector<element_t>& elements) override;
	void clear() { 
		reopen();
	}

private: 
	mutable std::mutex orphanageMutex;
	mutable std::mutex outMutex;
	std::vector<GroupInfo*> groups;

	// The bulk of the long-lived data is actually stored in here,
	// so be able to use mmap as storage.
	std::deque<std::vector<char, mmap_allocator<char>>, mmap_allocator<std::vector<char, mmap_allocator<char>>>> backingStore;

	// The orphanage stores nodes that come from groups that may be worked on by
	// multiple threads. They'll get folded into the index during finalize()
	std::map<NodeID, std::vector<element_t>> orphanage;
	std::vector<std::vector<element_t>> workerBuffers;
	void collectOrphans(const std::vector<element_t>& orphans);
	void publishGroup(const std::vector<element_t>& nodes);
};

#endif
