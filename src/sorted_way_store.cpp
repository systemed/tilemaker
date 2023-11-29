#include <iostream>
#include "sorted_way_store.h"

namespace SortedWayStoreTypes {
	thread_local bool collectingOrphans = true;
	thread_local uint64_t groupStart = -1;
	thread_local std::vector<std::pair<WayID, std::vector<NodeID>>>* localNodes = NULL;
}

using namespace SortedWayStoreTypes;


void SortedWayStore::reopen() {
	std::cout << "TODO: SortedWayStore::reopen()" << std::endl;
}

std::vector<LatpLon> SortedWayStore::at(WayID wayid) const {
	throw std::runtime_error("at() notimpl");
}

void SortedWayStore::insertLatpLons(std::vector<WayStore::ll_element_t> &newWays) {
	throw std::runtime_error("SortedWayStore does not support insertLatpLons");
}

const void SortedWayStore::insertNodes(const std::vector<std::pair<WayID, std::vector<NodeID>>>& newWays) {
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
