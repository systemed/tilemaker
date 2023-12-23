#ifndef DEQUE_MAP_H
#define DEQUE_MAP_H

#include <algorithm>
#include <boost/range/irange.hpp>
#include <cstring>
#include <deque>
#include <vector>

// A class which looks deep within the soul of some instance of
// a class T and assigns it a number based on the order in which
// it joined (or reminds it of its number).
//
// Used to translate an 8-byte pointer into a 4-byte ID that can be
// used repeatedly.
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
		// Search to see if we've already got this entry.
		const auto offsets = boost::irange<uint32_t>(0, keys.size());
		const auto it = std::lower_bound(
			offsets.begin(),
			offsets.end(),
			entry,
			[&](const auto &e, auto id) {
				return objects.at(keys[e]) < id;
			}
		);

		// We do, return its index.
		if (it != offsets.end() && objects[keys[*it]] == entry)
			return keys[*it];

		if (maxSize > 0 && objects.size() >= maxSize)
			return -1;

		// We don't, so store it...
		const uint32_t newIndex = objects.size();
		objects.push_back(entry);

		// ...and add its index to our keys vector.
		const uint32_t keysOffset = it == offsets.end() ? offsets.size() : *it;

		const uint32_t desiredSize = keys.size() + 1;

		// Amortize growth
		if (keys.capacity() < desiredSize)
			keys.reserve(keys.capacity() * 1.5);

		keys.resize(desiredSize);

		// Unless we're adding to the end, we need to shuffle existing keys down
		// to make room for our new index.
		if (keysOffset != newIndex) {
			std::memmove(&keys[keysOffset + 1], &keys[keysOffset], sizeof(uint32_t) * (keys.size() - 1 - keysOffset));
		}

		keys[keysOffset] = newIndex;
		return newIndex;
	}

	void clear() {
		objects.clear();
		keys.clear();
	}

	// Returns the index of `entry` if present, -1 otherwise.
	int32_t find(const T& entry) const {
		const auto offsets = boost::irange<uint32_t>(0, keys.size());
		const auto it = std::lower_bound(
			offsets.begin(),
			offsets.end(),
			entry,
			[&](const auto &e, auto id) {
				return objects.at(keys[e]) < id;
			}
		);

		// We do, return its index.
		if (it != offsets.end() && objects[keys[*it]] == entry)
			return keys[*it];

		return -1;
	}

	const T& at(uint32_t index) const {
		return objects.at(index);
	}

	size_t size() const { return objects.size(); }

	struct iterator {
		const DequeMap<T>& dm;
		size_t offset;
		iterator(const DequeMap<T>& dm, int offset): dm(dm), offset(offset) {}
		void operator++() { offset++; }
		bool operator!=(iterator& other) { return offset != other.offset; }
		const T& operator*() const { return dm.objects[dm.keys[offset]]; }
	};

	iterator begin() const { return iterator{*this, 0}; }
	iterator end() const { return iterator{*this, keys.size()}; }

private:
	uint32_t maxSize;

	// Using a deque is necessary, as it provides pointer-stability for previously
	// added objects when it grows the storage (as opposed to, e.g., vector).
	std::deque<T> objects;

	// Whereas `objects` is ordered by insertion-time, `keys` is sorted such that
	// objects[key[0]] < objects[key[1]] < ... < objects[key[$]]
	// operator< of T.
	std::vector<uint32_t> keys;
};
#endif
