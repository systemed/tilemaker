#ifndef _LEASED_STORE_H
#define _LEASED_STORE_H

#include "tile_data.h"
#include <tuple>

// LeasedStore implements "leases" for generated geometries.
//
// When Lua code calls Layer(...), we need to store the point, linestring,
// multilinestring, polygon geometry.
//
// Previously, we had code that would lock on each insertion:
//
// lock_guard<mutex> lock(mutex);
// insert(...)
//
// Instead, store leases let each of N threads claim a range of 1/N of the IDs.
// Then each thread can insert into their own stores without taking additional
// locks.

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
	std::vector<std::tuple<TileDataSource*, uint64_t, std::pair<size_t, T*>>> leases;	// source, its leaseNumber, lease

public:
	~LeasedStore() {
		// Only to the sources still alive (see registerLeaseSource)
		std::lock_guard<std::mutex> live(leaseSourcesMutex());
		for (const auto& lease : leases) {
			auto source = std::get<0>(lease);
			if (!leaseSourceAlive(source, std::get<1>(lease))) continue;
			std::lock_guard<std::mutex> lock(source->storeMutex);

			std::vector<std::pair<size_t, T*>>& availableLeases = getAvailableLeases<T>(source);
			availableLeases.push_back(std::get<2>(lease));
		}
	}

	std::pair<size_t, T*> get(TileDataSource* source) {
		for (const auto& lease : leases) {
			if (std::get<0>(lease) == source && std::get<1>(lease) == source->leaseNumber)
				return std::get<2>(lease);
		}

		std::lock_guard<std::mutex> lock(source->storeMutex);

		std::vector<std::pair<size_t, T*>>& availableLeases = getAvailableLeases<T>(source);

		if (availableLeases.empty())
			throw std::runtime_error("fatal: no available stores to lease");

		std::pair<size_t, T*> entry = availableLeases.back();
		availableLeases.pop_back();

		leases.push_back(std::make_tuple(source, source->leaseNumber, entry));
		return entry;
	}

};
#endif
