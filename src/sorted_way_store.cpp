#include <atomic>
#include <algorithm>
#include <bitset>
#include <iostream>
#include "sorted_way_store.h"

namespace SortedWayStoreTypes {
	const uint16_t GroupSize = 256;
	const uint16_t ChunkSize = 256;

	// We encode some things in the length of a way's unused upper bits.
	const uint16_t CompressedWay = 1 << 15;
	const uint16_t ClosedWay = 1 << 14;
	const uint16_t UniformUpperBits = 1 << 13;

	thread_local bool collectingOrphans = true;
	thread_local uint64_t groupStart = -1;
	thread_local std::vector<std::pair<WayID, std::vector<NodeID>>>* localWays = NULL;

	thread_local std::vector<uint8_t> encodedWay;

	std::atomic<uint64_t> totalWays;
	std::atomic<uint64_t> totalGroups;
	std::atomic<uint64_t> totalGroupSpace;
	std::atomic<uint64_t> totalChunks;
}

using namespace SortedWayStoreTypes;

SortedWayStore::SortedWayStore() {
	// Each group can store 64K ways. If we allocate 32K slots,
	// we support 2^31 = 2B ways, or about twice the number used
	// by OSM as of December 2023.
	groups.resize(32 * 1024);
}

void SortedWayStore::reopen() {
	std::cout << "TODO: SortedWayStore::reopen()" << std::endl;
}

std::vector<LatpLon> SortedWayStore::at(WayID wayid) const {
	std::vector<LatpLon> rv;
	return rv;
	//throw std::runtime_error("at() notimpl");
}

void SortedWayStore::insertLatpLons(std::vector<WayStore::ll_element_t> &newWays) {
	throw std::runtime_error("SortedWayStore does not support insertLatpLons");
}

const void SortedWayStore::insertNodes(const std::vector<std::pair<WayID, std::vector<NodeID>>>& newWays) {
	if (localWays == nullptr) {
		std::lock_guard<std::mutex> lock(orphanageMutex);
		if (workerBuffers.size() == 0)
			workerBuffers.reserve(256);
		else if (workerBuffers.size() == workerBuffers.capacity())
			throw std::runtime_error("SortedWayStore doesn't support more than 256 cores");
		workerBuffers.push_back(std::vector<std::pair<WayID, std::vector<NodeID>>>());
		localWays = &workerBuffers.back();
	}

	if (groupStart == -1) {
		// Mark where the first full group starts, so we know when to transition
		// out of collecting orphans.
		groupStart = newWays[0].first / (GroupSize * ChunkSize) * (GroupSize * ChunkSize);
	}

	int i = 0;
	while (collectingOrphans && i < newWays.size()) {
		const auto& el = newWays[i];
		if (el.first >= groupStart + (GroupSize * ChunkSize)) {
			collectingOrphans = false;
			// Calculate new groupStart, rounding to previous boundary.
			groupStart = el.first / (GroupSize * ChunkSize) * (GroupSize * ChunkSize);
			collectOrphans(*localWays);
			localWays->clear();
		}
		localWays->push_back(el);
		i++;
	}

	while(i < newWays.size()) {
		const auto& el = newWays[i];

		if (el.first >= groupStart + (GroupSize * ChunkSize)) {
			publishGroup(*localWays);
			localWays->clear();
			groupStart = el.first / (GroupSize * ChunkSize) * (GroupSize * ChunkSize);
		}

		localWays->push_back(el);
		i++;
	}
}

void SortedWayStore::clear() {
	std::cout << "TODO: SortedWayStore::clear()" << std::endl;
}

std::size_t SortedWayStore::size() const {
	return totalWays.load();
}

void SortedWayStore::finalize(unsigned int threadNum) {
	for (const auto& buffer: workerBuffers) {
		if (buffer.size() > 0) {
			collectOrphans(buffer);
		}
	}
	workerBuffers.clear();

	// Empty the orphanage into the index.
	std::vector<std::pair<WayID, std::vector<NodeID>>> copy;
	for (const auto& entry: orphanage) {
		for (const auto& orphan: entry.second)
			copy.push_back(orphan);

		// Orphans may come from different workers, and thus be unsorted.
		std::sort(
			copy.begin(),
			copy.end(), 
			[](auto const &a, auto const &b) { return a.first < b.first; }
		);
		publishGroup(copy);
		copy.clear();
	}

	orphanage.clear();

	std::cout << "SortedWayStore: " << totalGroups << " groups, " << totalChunks << " chunks, " << totalWays.load() << " ways, " << totalGroupSpace.load() << " bytes" << std::endl;
	/*
	for (int i = 0; i < 257; i++)
		std::cout << "chunkSizeFreqs[ " << i << " ]= " << chunkSizeFreqs[i].load() << std::endl;
	for (int i = 0; i < 257; i++)
		std::cout << "groupSizeFreqs[ " << i << " ]= " << groupSizeFreqs[i].load() << std::endl;
		*/

}

void SortedWayStore::batchStart() {
	collectingOrphans = true;
	groupStart = -1;
	if (localWays == nullptr || localWays->size() == 0)
		return;

	collectOrphans(*localWays);
	localWays->clear();
}

void SortedWayStore::collectOrphans(const std::vector<std::pair<WayID, std::vector<NodeID>>>& orphans) {
	std::lock_guard<std::mutex> lock(orphanageMutex);
	size_t groupIndex = orphans[0].first / (GroupSize * ChunkSize);

	std::vector<std::pair<WayID, std::vector<NodeID>>>& vec = orphanage[groupIndex];
	const size_t i = vec.size();
	vec.resize(i + orphans.size());
	std::copy(orphans.begin(), orphans.end(), vec.begin() + i);
}

std::vector<NodeID> SortedWayStore::decodeWay(uint16_t flags, const uint8_t* input) {
	std::vector<NodeID> rv;

	bool isCompressed = flags & CompressedWay;
	
	if (isCompressed)
		throw std::runtime_error("cannot decode compressed ways yet");

	bool isClosed = flags & ClosedWay;

	const uint16_t length = flags & 0b0000011111111111;

	// TODO: handle isCompressed

	uint64_t highBytes[length];

	if (!(flags & UniformUpperBits)) {
		// The nodes don't all share the same upper int; unpack which
		// bits are set on a per-node basis.
		for (int i = 0; i <= (length - 1) / 4; i++) {
			uint8_t byte = *input;
			for (int j = i * 4; j < std::min<int>(length, i * 4 + 4); j++) {
				uint64_t highByte = 0;
				highByte |= (byte & 0b00000011);
				byte = byte >> 2;
				highBytes[j] = (highByte << 32);
			}
			input++;
		}
	} else {
		uint8_t setBits = (flags >> 11) & 0b00000011;
		uint64_t highByte = setBits << 32;
		for (int i = 0; i < length; i++)
			highBytes[i] = highByte;
	}

	// Decode the low ints
	uint32_t* lowIntData = (uint32_t*)input;
	for (int i = 0; i < length; i++)
		rv.push_back(highBytes[i] | lowIntData[i]);

	if (isClosed)
		rv.push_back(rv[0]);
	return rv;
};

uint16_t SortedWayStore::encodeWay(const std::vector<NodeID>& way, std::vector<uint8_t>& output, bool compress) {
	if (compress)
		throw std::runtime_error("Way compression not implemented yet");

	if (way.size() == 0)
		throw std::runtime_error("Cannot encode an empty way");

	if (way.size() > 2000)
		throw std::runtime_error("Way had more than 2,000 nodes");

	bool isClosed = way.size() > 1 && way[0] == way[way.size() - 1];
	output.clear();

	// When the way is closed, store that in a single bit and omit
	// the final point.
	const int max = isClosed ? way.size() - 1 : way.size();

	uint16_t rv = max;

	if (isClosed)
		rv |= ClosedWay;

	bool pushUpperBits = false;
	uint32_t upperInt = way[0] >> 32;
	for (int i = 1; i < way.size(); i++) {
		if (way[i] >> 32 != upperInt) {
			pushUpperBits = true;
			break;
		}
	}

	if (pushUpperBits) {
		for (int i = 0; i <= (max - 1) / 4; i++) {
			uint8_t byte = 0;

			//for (j = i * 4; j < std::min(max, i * 4 + 4); j++) {
			bool first = true;
			for (int j = std::min(max, i * 4 + 4) - 1; j >= i * 4; j--) {
				if (!first)
					byte = byte << 2;
				first = false;
				uint8_t upper2Bits = way[j] >> 32;
				if (upper2Bits > 3)
					throw std::runtime_error("unexpectedly high node ID: " + std::to_string(way[j]));
				byte |= upper2Bits;
			}

			output.push_back(byte);
		}
	} else {
		if (upperInt > 3)
			throw std::runtime_error("unexpectedly high node ID");

		rv |= UniformUpperBits;
		rv |= (upperInt << 11);
	}

	// Push the low bytes.
	const size_t oldSize = output.size();
	output.resize(output.size() + max * 4);
	uint32_t* dataStart = (uint32_t*)(output.data() + oldSize);
	for (int i = 0; i < max; i++) {
		uint32_t lowBits = way[i];
		dataStart[i] = lowBits;
	}

	return rv;
}

void SortedWayStore::publishGroup(const std::vector<std::pair<WayID, std::vector<NodeID>>>& ways) {
	totalWays += ways.size();
	if (ways.size() == 0) {
		throw std::runtime_error("SortedWayStore: group is empty");
	}
	size_t groupIndex = ways[0].first / (GroupSize * ChunkSize);

	if (ways.size() > ChunkSize * GroupSize) {
		std::cout << "groupIndex=" << groupIndex << ", first ID=" << ways[0].first << ", ways.size() = " << ways.size() << std::endl;
		throw std::runtime_error("SortedWayStore: group is too big");
	}

	totalGroups++;

	for (const auto& way : ways) {
		const WayID id = way.first;
		const std::vector<NodeID>& nodes = way.second;

		// Encode the way, uncompressed.
		encodeWay(way.second, encodedWay, false);
	}


}



