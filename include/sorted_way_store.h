#ifndef _SORTED_WAY_STORE_H
#define _SORTED_WAY_STORE_H

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include "way_store.h"
#include "mmap_allocator.h"

class NodeStore;

// Like SortedNodeStore, but for ways.
//
// Ways are variable length, whereas nodes are a fixed 8 bytes.
//
// This is important for two reasons:
// - we were able to directly calculate the offset of the node in a chunk (the size is fixed)
// - we could fit the offsets of chunk in a short (the size is small)
//
// Per https://wiki.openstreetmap.org/wiki/Way, a way can have at most 2,000 nodes.
//
// In practice, most ways have far fewer than 2,000 nodes.
//   for NS: p50=7, p90=32, p95=54, p99=161
//   for GB: p50=5, p90=19, p95=30, p99=82
//   for ON: p50=8, p90=31, p95=54, p99=172
//
// That is, 50% of the time, ways have 8 or fewer nodes. 90% of the time,
// they have 32 or fewer nodes.

namespace SortedWayStoreTypes {

	struct EncodedWay {
		// A way can have 2000 nodes.
		// Bits 0..10 track how many nodes are in this way.
		// That leaves 5 bits for activities:
		// ab0xx: bits 31..34 of node ID are interwoven as bytes.
		// ab1xx: bits 31..34 of node ID are the same, stored as first byte
		//
		// 1xxxx: This way is stored zigzag encoded.
		// z1zzz: This is a closed way, repeat the first node as the last node.
		//
		// When it's compressed, we still handle high bits the same,
		// but the low bytes are compressed.
		//
		// We'd need to add a compressedLength, but otherwise it'd
		// be the same.
		uint16_t flags;
		// Data could be:
		// (if interwoven bit) N/2 bytes: interwoven high bits
		// (if compression bit) 2 bytes: compressed length
		// (if compression bit) 4 bytes: first 32-bit value
		// N 32-bit ints: the N low ints
		uint8_t data[0];
	};

	struct ChunkInfo {
		// Bitmasks indicating which ways are in this chunk.
		// Small ways: these are ways that can be stored in 256 bytes or less,
		//   they can be identified with a scale of 1 relative to end of wayOffsets.
		//
		//   We expect 60-80% of ways to be small ways.
		//
		// Big ways: these are ways that require more than 256 bytes,
		//   they can be identified with a scale of 64 relative to start of chunk.
		uint8_t smallWayMask[32];
		uint8_t bigWayMask[32];

		uint16_t wayOffsets[0];
	};

	struct GroupInfo {
		// A bitmask indicating how many chunks are in this group.
		uint8_t chunkMask[32];

		// There is an entry for each set bit in chunkMask. They identify
		// the address of a ChunkInfo. The address is relative to the end
		// of the GroupInfo struct.
		uint32_t chunkOffsets[0];
	};
}

class SortedWayStore: public WayStore {

public:
	SortedWayStore(bool compressWays, const NodeStore& nodeStore);
	~SortedWayStore();
	void reopen() override;
	void batchStart() override;
	std::vector<LatpLon> at(WayID wayid) const override;
	bool requiresNodes() const override { return true; }
	void insertLatpLons(std::vector<WayStore::ll_element_t> &newWays) override;
	const void insertNodes(const std::vector<std::pair<WayID, std::vector<NodeID>>>& newWays) override;
	void clear() override;
	std::size_t size() const override;
	void finalize(unsigned int threadNum) override;

	bool contains(size_t shard, WayID id) const override;
	size_t shard() const override { return 0; }
	size_t shards() const override { return 1; }
	
	static uint16_t encodeWay(
		const std::vector<NodeID>& way,
		std::vector<uint8_t>& output,
		bool compress
	);

	static std::vector<NodeID> decodeWay(uint16_t flags, const uint8_t* input);

private:
	bool compressWays;
	const NodeStore& nodeStore;
	mutable std::mutex orphanageMutex;
	std::vector<SortedWayStoreTypes::GroupInfo*> groups;
	std::vector<std::pair<void*, size_t>> allocatedMemory;

	// The orphanage stores nodes that come from groups that may be worked on by
	// multiple threads. They'll get folded into the index during finalize()
	std::map<WayID, std::vector<std::pair<WayID, std::vector<NodeID>>>> orphanage;
	std::vector<std::vector<std::pair<WayID, std::vector<NodeID>>>> workerBuffers;

	std::atomic<uint64_t> totalWays;
	std::atomic<uint64_t> totalNodes;
	std::atomic<uint64_t> totalGroups;
	std::atomic<uint64_t> totalGroupSpace;
	std::atomic<uint64_t> totalChunks;

	void collectOrphans(const std::vector<std::pair<WayID, std::vector<NodeID>>>& orphans);
	void publishGroup(const std::vector<std::pair<WayID, std::vector<NodeID>>>& ways);
};


// TODO: consider extracting this for SortedNodeStore if we rewrite that class
void populateMask(uint8_t* mask, const std::vector<uint8_t>& ids);

#endif
