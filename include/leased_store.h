#ifndef _STORE_LEASE_H
#define _STORE_LEASE_H

#include "tile_data.h"

template<typename S>
std::vector<std::pair<size_t, S*>>& getAvailableLeases(TileDataSource* source) {
	throw std::runtime_error("you need to specialize this");
}

template<>
inline std::vector<std::pair<size_t, TileDataSource::point_store_t*>>& getAvailableLeases(TileDataSource* source) {
	return source->availablePointStoreLeases;
}

template<>
inline std::vector<std::pair<size_t, TileDataSource::linestring_store_t*>>& getAvailableLeases(TileDataSource* source) {
	return source->availableLinestringStoreLeases;
}

template<>
inline std::vector<std::pair<size_t, TileDataSource::multi_linestring_store_t*>>& getAvailableLeases(TileDataSource* source) {
	return source->availableMultiLinestringStoreLeases;
}

template<>
inline std::vector<std::pair<size_t, TileDataSource::multi_polygon_store_t*>>& getAvailableLeases(TileDataSource* source) {
	return source->availableMultiPolygonStoreLeases;
}


template <class T>
class LeasedStore {
	std::vector<std::pair<TileDataSource*, std::pair<size_t, T*>>> leases;

public:
	~LeasedStore() {
		for (const auto& lease : leases) {
			auto source = lease.first;
			std::lock_guard<std::mutex> lock(source->storeMutex);

			std::vector<std::pair<size_t, T*>>& availableLeases = getAvailableLeases<T>(source);
			availableLeases.push_back(lease.second);
		}
	}

	std::pair<size_t, T*> get(TileDataSource* source) {
		for (const auto& lease : leases) {
			if (lease.first == source)
				return lease.second;
		}

		std::lock_guard<std::mutex> lock(source->storeMutex);

		std::vector<std::pair<size_t, T*>>& availableLeases = getAvailableLeases<T>(source);

		if (availableLeases.empty())
			throw std::runtime_error("fatal: no available stores to lease");

		std::pair<size_t, T*> entry = availableLeases.back();
		availableLeases.pop_back();

		leases.push_back(std::make_pair(source, entry));
		return entry;
	}

};
#endif
