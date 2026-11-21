#ifndef DEQUE_MAP_H
#define DEQUE_MAP_H

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

// A class which looks deep within the soul of some instance of
// a class T and assigns it a number based on the order in which
// it joined (or reminds it of its number).
//
// Used to translate an 8-byte pointer into a 4-byte ID that can be
// used repeatedly.
//
// T needs operator== and operator<, and a hash() member (std::string uses std::hash).
template <class T>
class DequeMap {
public:
	DequeMap(): maxSize(0) {}
	DequeMap(uint32_t maxSize): maxSize(maxSize) {}

	bool full() const {
		return maxSize != 0 && size() == maxSize;
	}

	// If `entry` is already in the map, return its index.
	// Otherwise, if maxSize is `0`, or greater than the number of entries in the map,
	// add the item and return its index.
	// Otherwise, return -1.
	int32_t add(const T& entry) {
		return addWithHash(entry, rawHash(entry));
	}

	// As add/find, with rawHash = entry.hash() (std::hash for std::string) given
	int32_t addWithHash(const T& entry, uint64_t rawHash) {
		const uint64_t h = mix(rawHash);
		const int32_t found = lookup(entry, h);
		if (found >= 0)
			return found;
		return addNew(entry, h);
	}
	int32_t findWithHash(const T& entry, uint64_t rawHash) const {
		return lookup(entry, mix(rawHash));
	}
	// As addWithHash, for an entry known not to be in the map
	int32_t addAbsentWithHash(const T& entry, uint64_t rawHash) {
		return addNew(entry, mix(rawHash));
	}

private:
	int32_t addNew(const T& entry, uint64_t h) {
		if (maxSize > 0 && objects.size() >= maxSize)
			return -1;

		// We don't, so store it...
		const uint32_t newIndex = objects.size();
		objects.push_back(entry);
		sortedValid = false;

		// ...and index it, growing the table to keep it at most half full
		if (2 * (objects.size() + 1) > slots.size()) {
			std::vector<uint32_t> old(std::max<size_t>(16, slots.size() * 2), 0);
			slots.swap(old);
			for (uint32_t i = 0; i < newIndex; i++)
				insertSlot(hashOf(objects[i]), i);
		}
		insertSlot(h, newIndex);
		return newIndex;
	}

public:
	void clear() {
		objects.clear();
		slots.clear();
		sorted.clear();
		sortedValid = false;
	}

	// Returns the index of `entry` if present, -1 otherwise.
	int32_t find(const T& entry) const {
		return lookup(entry, hashOf(entry));
	}

	inline const T& operator[](uint32_t index) const {
		return objects[index];
	}

	inline const T& at(uint32_t index) const {
		return objects.at(index);
	}

	size_t size() const { return objects.size(); }

	// Iterates in the order of T's operator<
	struct iterator {
		const DequeMap<T>& dm;
		size_t offset;
		iterator(const DequeMap<T>& dm, size_t offset): dm(dm), offset(offset) {}
		void operator++() { offset++; }
		bool operator!=(iterator& other) { return offset != other.offset; }
		const T& operator*() const { return dm.objects[dm.sorted[offset]]; }
	};

	iterator begin() const {
		if (!sortedValid) {
			sorted.resize(objects.size());
			for (uint32_t i = 0; i < sorted.size(); i++) sorted[i] = i;
			std::sort(sorted.begin(), sorted.end(), [&](uint32_t a, uint32_t b) { return objects[a] < objects[b]; });
			sortedValid = true;
		}
		return iterator{*this, 0};
	}
	iterator end() const { return iterator{*this, objects.size()}; }

private:
	template <class U> static uint64_t rawHash(const U& entry) { return entry.hash(); }
	static uint64_t rawHash(const std::string& entry) { return std::hash<std::string>()(entry); }

	// The entries of one DequeMap often share bits of their hash (sharded by it), so mix it
	static uint64_t hashOf(const T& entry) {
		return mix(rawHash(entry));
	}
	static uint64_t mix(uint64_t h) {
		h ^= h >> 33; h *= 0xff51afd7ed558ccdULL;
		h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL;
		h ^= h >> 33;
		return h;
	}

	int32_t lookup(const T& entry, uint64_t h) const {
		if (slots.empty()) return -1;
		const size_t mask = slots.size() - 1;
		for (size_t i = h & mask; slots[i] != 0; i = (i + 1) & mask)
			if (objects[slots[i] - 1] == entry)
				return slots[i] - 1;
		return -1;
	}

	void insertSlot(uint64_t h, uint32_t index) {
		const size_t mask = slots.size() - 1;
		size_t i = h & mask;
		while (slots[i] != 0) i = (i + 1) & mask;
		slots[i] = index + 1;
	}

	uint32_t maxSize;

	// Using a deque is necessary, as it provides pointer-stability for previously
	// added objects when it grows the storage (as opposed to, e.g., vector).
	std::deque<T> objects;

	// Open-addressing hash table of the objects: index + 1, 0 = empty
	std::vector<uint32_t> slots;

	// For iteration only: object indexes in the order of operator<
	mutable std::vector<uint32_t> sorted;
	mutable bool sortedValid = false;
};
#endif
