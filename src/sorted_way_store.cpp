#include <iostream>
#include "sorted_way_store.h"

void SortedWayStore::reopen() {
	std::cout << "TODO: SortedWayStore::reopen()" << std::endl;
}

const SortedWayStore::latplon_vector_t& SortedWayStore::at(WayID wayid) const {
	throw std::runtime_error("at() notimpl");
}

void SortedWayStore::insert(std::vector<element_t> &newWays) {
	throw std::runtime_error("insert() notimpl");
}

void SortedWayStore::clear() {
	std::cout << "TODO: SortedWayStore::clear()" << std::endl;
}

std::size_t SortedWayStore::size() const {
	throw std::runtime_error("size() notimpl");
}

void SortedWayStore::finalize(unsigned int threadNum) {
	throw std::runtime_error("finalize() notimpl");
}
