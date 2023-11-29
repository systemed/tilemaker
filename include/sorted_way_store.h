#ifndef _SORTED_WAY_STORE_H
#define _SORTED_WAY_STORE_H

#include <memory>
#include <mutex>
#include "way_store.h"
#include "mmap_allocator.h"

class SortedWayStore: public WayStore {

public:
	void reopen() override;
	const WayStore::latplon_vector_t& at(WayID wayid) const override;
	bool requiresNodes() const override { return true; }
	void insertLatpLons(std::vector<WayStore::ll_element_t> &newWays) override;
	const void insertNodes(const std::vector<std::pair<WayID, std::vector<NodeID>>>& newWays) override;
	void clear() override;
	std::size_t size() const override;
	void finalize(unsigned int threadNum) override;

private:
	mutable std::mutex mutex;
};

#endif
