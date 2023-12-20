#ifndef _WAY_STORES_H
#define _WAY_STORES_H

#include <memory>
#include <mutex>
#include "way_store.h"
#include "sorted_way_store.h"

class BinarySearchWayStore: public WayStore {

public:
	using map_t = std::deque<WayStore::ll_element_t, mmap_allocator<WayStore::ll_element_t>>;

	void reopen() override;
	void batchStart() override {}
	std::vector<LatpLon> at(WayID wayid) const override;
	bool requiresNodes() const override { return false; }
	void insertLatpLons(std::vector<WayStore::ll_element_t> &newWays) override;
	const void insertNodes(const std::vector<std::pair<WayID, std::vector<NodeID>>>& newWays) override;
	void clear() override;
	std::size_t size() const override;
	void finalize(unsigned int threadNum) override;

private:
	mutable std::mutex mutex;
	std::unique_ptr<map_t> mLatpLonLists;
};

#endif
