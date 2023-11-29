#ifndef _SORTED_WAY_STORE_H
#define _SORTED_WAY_STORE_H

#include <map>
#include <memory>
#include <mutex>
#include "way_store.h"
#include "mmap_allocator.h"

class SortedWayStore: public WayStore {

public:
	void reopen() override;
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
	void publishGroup(const std::vector<std::pair<WayID, std::vector<NodeID>>>& nodes);
};

#endif
