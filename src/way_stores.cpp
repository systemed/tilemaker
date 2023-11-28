#include <boost/sort/sort.hpp>

#include "way_stores.h"

void BinarySearchWayStore::finalize(unsigned int threadNum) { 
	std::lock_guard<std::mutex> lock(mutex);
	boost::sort::block_indirect_sort(
		mLatpLonLists->begin(), mLatpLonLists->end(), 
		[](auto const &a, auto const &b) { return a.first < b.first; }, 
		threadNum);
}

void BinarySearchWayStore::reopen() {
	mLatpLonLists = std::make_unique<map_t>();
}

const BinarySearchWayStore::latplon_vector_t& BinarySearchWayStore::at(WayID wayid) const {
	std::lock_guard<std::mutex> lock(mutex);
	
	auto iter = std::lower_bound(mLatpLonLists->begin(), mLatpLonLists->end(), wayid, [](auto const &e, auto wayid) { 
		return e.first < wayid; 
	});

	if(iter == mLatpLonLists->end() || iter->first != wayid)
		throw std::out_of_range("Could not find way with id " + std::to_string(wayid));

	return iter->second;
}

void BinarySearchWayStore::insert(std::vector<element_t> &newWays) {
	std::lock_guard<std::mutex> lock(mutex);
	auto i = mLatpLonLists->size();
	mLatpLonLists->resize(i + newWays.size());
	std::copy(std::make_move_iterator(newWays.begin()), std::make_move_iterator(newWays.end()), mLatpLonLists->begin() + i); 
}

void BinarySearchWayStore::clear() {
	std::lock_guard<std::mutex> lock(mutex);
	mLatpLonLists->clear(); 
}

std::size_t BinarySearchWayStore::size() const {
	std::lock_guard<std::mutex> lock(mutex);
	return mLatpLonLists->size(); 
}
