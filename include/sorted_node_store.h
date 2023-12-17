#ifndef _SORTED_NODE_STORE_H
#define _SORTED_NODE_STORE_H

#include "node_store.h"
#include "mmap_allocator.h"
#include <atomic>
#include <map>
#include <memory>
#include <mutex>

// SortedNodeStore requires the Sort.Type_then_ID property on the source PBF.
//
// It stores nodes in chunks of 256, and chunks in groups of 256.
// Access to a node given its NodeID is constant time.
//
// Additional memory usage varies, approaching 1% for very large PBFs.

namespace SortedNodeStoreTypes {
	struct ChunkInfoBase {
		// If high bit is set, this is a compressed chunk.
		// Bits 0..9 are the length of the compressed lons.
		// Bits 10..19 are the length of the compressed lats.
		// The upper-most bit should be set iff this is a compressed chunk.
		uint32_t flags;
		// A bitmask indicating how many nodes are in this chunk.
		uint8_t nodeMask[32];
	};

	struct CompressedChunkInfo: ChunkInfoBase {
		// streamvbyte_decode needs N, the size of the original array.
		// N is popcnt(nodeMask) - 1.
		// data is zigzag delta encoded, so we need firstLatp and firstLatp to recover it.
		int32_t firstLatp;
		int32_t firstLon;
		uint8_t data[0];
	};

	struct UncompressedChunkInfo: ChunkInfoBase {
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
}


class SortedNodeStore : public NodeStore
{

public:
	SortedNodeStore(bool compressNodes);
	~SortedNodeStore();
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
	size_t shard() const override { return 0; }
	size_t shards() const override { return 1; }

private: 
	// When true, store chunks compressed. Only store compressed if the
	// chunk is sufficiently large.
	bool compressNodes;

	mutable std::mutex orphanageMutex;
	std::vector<SortedNodeStoreTypes::GroupInfo*> groups;
	std::vector<std::pair<void*, size_t>> allocatedMemory;

	// The orphanage stores nodes that come from groups that may be worked on by
	// multiple threads. They'll get folded into the index during finalize()
	std::map<NodeID, std::vector<element_t>> orphanage;
	std::vector<std::vector<element_t>> workerBuffers;

	std::atomic<uint64_t> totalGroups;
	std::atomic<uint64_t> totalNodes;
	std::atomic<uint64_t> totalGroupSpace;
	std::atomic<uint64_t> totalAllocatedSpace;
	std::atomic<uint64_t> totalChunks;
	std::atomic<uint64_t> chunkSizeFreqs[257];
	std::atomic<uint64_t> groupSizeFreqs[257];

	void collectOrphans(const std::vector<element_t>& orphans);
	void publishGroup(const std::vector<element_t>& nodes);
};

#endif
