#ifndef _SORTED_WAY_STORE_H
#define _SORTED_WAY_STORE_H

#include <map>
#include <memory>
#include <mutex>
#include "way_store.h"
#include "mmap_allocator.h"

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
// In the naive case:
// - we store each node ID as a 64-bit value
// - we store the length, which takes 11 bits
// ...a way could take 2 + 2000*8 = 16,002 byte
// ...4 ways could fit in a short.
//
// Option 1: up to 256 uint32_ts => 1,024 bytes
//   pro: simple, and 4 bytes is still better than a vector's 24 bytes.
//
// Option 2: 64 uint32_ts, plus 256 uint16_ts => 768 bytes
//   pro: better memory usage
//   con: fiddly
//
// Option 3: break ways into the component parts that would fit in a short.
//           eg 64K / 256 => 256 => break a way into 32-node chunks.
//  pro: very memory efficient. most (how many?) ways are likely <= 32 nodes.
//
// Option 4: require a running tally, e.g. store relative offsets. This
//           feels a bit shitty on perf.

class SortedWayStore: public WayStore {

public:
	void reopen() override;
	void batchStart() override;
	std::vector<LatpLon> at(WayID wayid) const override;
	bool requiresNodes() const override { return true; }
	void insertLatpLons(std::vector<WayStore::ll_element_t> &newWays) override;
	const void insertNodes(const std::vector<std::pair<WayID, std::vector<NodeID>>>& newWays) override;
	void clear() override;
	std::size_t size() const override;
	void finalize(unsigned int threadNum) override;

private:
	mutable std::mutex orphanageMutex;

	// The orphanage stores nodes that come from groups that may be worked on by
	// multiple threads. They'll get folded into the index during finalize()
	std::map<WayID, std::vector<std::pair<WayID, std::vector<NodeID>>>> orphanage;
	std::vector<std::vector<std::pair<WayID, std::vector<NodeID>>>> workerBuffers;
	void collectOrphans(const std::vector<std::pair<WayID, std::vector<NodeID>>>& orphans);
	void publishGroup(const std::vector<std::pair<WayID, std::vector<NodeID>>>& ways);
};

#endif
