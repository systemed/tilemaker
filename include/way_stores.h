#ifndef _WAY_STORES_H
#define _WAY_STORES_H

#include <memory>
#include <mutex>
#include "way_store.h"

class BinarySearchWayStore: public WayStore {

public:
	using latplon_vector_t = std::vector<LatpLon, mmap_allocator<LatpLon>>;
	using element_t = std::pair<WayID, latplon_vector_t>;
	using map_t = std::deque<element_t, mmap_allocator<element_t>>;

	void reopen() override;
	const latplon_vector_t& at(WayID wayid) const override;
	void insert(std::vector<element_t> &newWays) override;
	void clear() override;
	std::size_t size() const override;
	void finalize(unsigned int threadNum) override;

private:
	mutable std::mutex mutex;
	std::unique_ptr<map_t> mLatpLonLists;
};

#endif
