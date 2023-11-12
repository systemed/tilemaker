/*! \file */ 
#ifndef _ATTRIBUTE_STORE_H
#define _ATTRIBUTE_STORE_H

#include "vector_tile.pb.h"
#include <mutex>
#include <atomic>
#include <boost/functional/hash.hpp>
#include <vector>
#include <unordered_map>
#include <tsl/ordered_set.h>
#include <random>
#include <iostream>

inline std::ostream& operator<<(std::ostream& os, const vector_tile::Tile_Value& value) {
	if (value.has_string_value()) os << "[str]" << value.string_value();
	if (value.has_bool_value()) os << "[bool]" << value.bool_value();
	if (value.has_float_value()) os << "[float]" << value.float_value();
	return os;
}


/* AttributeStore - global dictionary for attributes */

typedef uint32_t AttributeIndex; // check this is enough

// All members of this class are thread-safe.
//
// AttributeKeyStore maintains a pointer to the live version.
// Lookup misses will result in a new version being published.
class AttributeKeyStoreImmutable {
public:
	AttributeKeyStoreImmutable(std::map<const std::string, uint16_t> keys2index): keys2index(keys2index) {
	}

	uint16_t key2index(const std::string& key) {
		auto rv = keys2index.find(key);

		if (rv == keys2index.end())
			// 0 acts as a sentinel to say that it's missing.
			return 0;

		return rv->second;
	}

	const std::map<const std::string, uint16_t> getKeys2IndexMap() { return keys2index; }

private:
	std::map<const std::string, uint16_t> keys2index;
};

class AttributeKeyStore {
public:
	// We jump through some hoops to have no locks for most readers,
	// locking only if we need to add the value.
	static uint16_t key2index(const std::string& key) {
		auto index = immutable->key2index(key);

		if (index != 0)
			return index;

		std::lock_guard<std::mutex> lock(keys2index_mutex);

		// 0 is used as a sentinel, so ensure that the 0th element is just a dummy element.
		if (keys.size() == 0)
			keys.push_back("");

		// Double-check that it's not there - maybe we were in a race
		auto reallyMissing = immutable->key2index(key);
		if (reallyMissing != 0)
			return reallyMissing;

		uint16_t newIndex = keys.size();

		// This is very unlikely. We expect more like 50-100 keys.
		if (newIndex >= 65535)
			throw std::out_of_range("more than 65,536 unique keys");

		std::map<const std::string, uint16_t> newMap(immutable->getKeys2IndexMap());
		newMap[key] = newIndex;
		keys.push_back(key);

		immutable = std::make_unique<AttributeKeyStoreImmutable>(newMap);
		return newIndex;
	}

	static const std::string& getKey(uint16_t index) {
		return keys[index];
	}

private:
	static std::mutex keys2index_mutex;
	static std::unique_ptr<AttributeKeyStoreImmutable> immutable;
	// NB: we use a deque, not a vector, because a deque never invalidates
	// pointers to its members as long as you only push_back
	static std::deque<std::string> keys;
};

// AttributePair is a key/value pair (with minzoom)
struct AttributePair {
	vector_tile::Tile_Value value;
	short keyIndex;
	char minzoom;

	AttributePair(std::string const &key, vector_tile::Tile_Value const &value, char minzoom)
		: keyIndex(AttributeKeyStore::key2index(key)), value(value), minzoom(minzoom)
	{ }

	bool operator==(const AttributePair &other) const {
		if (minzoom!=other.minzoom || keyIndex!=other.keyIndex) return false;
		if (value.has_string_value()) return other.value.has_string_value() && other.value.string_value()==value.string_value();
		if (value.has_bool_value())   return other.value.has_bool_value()   && other.value.bool_value()  ==value.bool_value();
		if (value.has_float_value())  return other.value.has_float_value()  && other.value.float_value() ==value.float_value();
		throw std::runtime_error("Invalid type in attribute store");
	}

	const std::string& key() const {
		return AttributeKeyStore::getKey(keyIndex);
	}

	enum class Index { BOOL, FLOAT, STRING };
	static Index type_index(vector_tile::Tile_Value const &v) {
		if     (v.has_string_value()) return Index::STRING;
		else if(v.has_float_value())  return Index::FLOAT;
		else                          return Index::BOOL;
	}

	size_t hash() const {
		std::size_t rv = minzoom;
		boost::hash_combine(rv, keyIndex);
		boost::hash_combine(rv, type_index(value));

		if(value.has_string_value())
			boost::hash_combine(rv, value.string_value());
		else if(value.has_float_value())
			boost::hash_combine(rv, value.float_value());
		else
			boost::hash_combine(rv, value.bool_value());

		return rv;
	}
};


// Pick SHARD_BITS such that PAIR_SHARDS is at least 2x your number of cores.
// This reduces the odds of lock contention on inserting/retrieving the "cold" pairs.
// For now, it's hardcoded, perhaps it should be a function of the numThreads argument.
#define SHARD_BITS 7
#define PAIR_SHARDS (1 << SHARD_BITS)

class AttributePairStore {
public:
	static const AttributePair& getPair(uint32_t i) {
		uint32_t shard = i >> (32 - SHARD_BITS);
		uint32_t offset = i & (~(~0u << (32 - SHARD_BITS)));

		std::lock_guard<std::mutex> lock(pairRefs_mutex[shard]);
		return pairRefs[shard][offset];
	};

	static uint32_t addPair(const AttributePair& pair);

private:
	// We refer to all attribute pairs by index. To avoid contention,
	// we shard the deques containing the pairs. If there are two shards,
	// shard 1 starts from 0, shard 2 starts from 2^31, and so on.
	// TODO: optimize for "hot" pairs, which will be stored in the first 64K entries,
	//       so that we can refer to them by a short.
	static std::vector<std::deque<AttributePair>> pairRefs;
	static std::vector<std::mutex> pairRefs_mutex;

};

// AttributeSet is a set of AttributePairs
// = the complete attributes for one object
struct AttributeSet {
	static bool compare(vector_tile::Tile_Value const &lhs, vector_tile::Tile_Value const &rhs) {
		auto lhs_id = AttributePair::type_index(lhs);
		auto rhs_id = AttributePair::type_index(lhs);
		if(lhs_id < rhs_id) return true;
		if(lhs_id > rhs_id) return false;
		switch(lhs_id) {
			case AttributePair::Index::BOOL:    return lhs.bool_value() < rhs.bool_value();
			case AttributePair::Index::FLOAT:   return lhs.float_value() < rhs.float_value();
			case AttributePair::Index::STRING:  return lhs.string_value() < rhs.string_value();
		}
		throw std::runtime_error("Invalid type in attribute store");
	}

    struct key_value_less {
        bool operator()(AttributePair const &lhs, AttributePair const& rhs) const {            
			return (lhs.minzoom != rhs.minzoom) ? (lhs.minzoom < rhs.minzoom)
			     : (lhs.keyIndex != rhs.keyIndex) ? (lhs.keyIndex < rhs.keyIndex)
			     : compare(lhs.value, rhs.value);
        }
    }; 

	struct hash_function {
		// Calculating the hash value requires indirection and locks, so
		// we use a memoized version calculated once the AttributeSet is finalized.
		size_t operator()(const AttributeSet &attributes) const {
			return attributes.hash_value;
		}
	};
	bool operator==(const AttributeSet &other) const {
		if (hash_value == 0 && values.size() != 0)
			std::cout << "unexpected hash value of 0 for this, values.size()=" << values.size() << std::endl;
		if (other.hash_value == 0 && other.values.size() != 0) {
			std::cout << "unexpected hash value of 0 for other, other.values.size()=" << other.values.size() << std::endl;
		}

		if (hash_value != other.hash_value)
			return false;

		if (values.size() != other.values.size())
			return false;

		// Equivalent if, for every value in values, there is a value in other.values
		// whose pair is the same.
		// TODO: finalize_set ought to ensure that everyone's values are sorted such
		//  that we can just do a pairwise comparison.

		for (const auto& myValue: values) {
			bool ok = false;
			const auto& myPair = AttributePairStore::getPair(myValue);
			for (const auto& theirValue: other.values) {
				const auto& theirPair = AttributePairStore::getPair(theirValue);

				if (myPair == theirPair) {
					ok = true;
					break;
				}
			}

			if (!ok)
				return false;
		}

		return true;
	}

	void finalize_set();

	std::vector<uint32_t> values;

	void add(AttributePair const &kv);
	void add(std::string const &key, vector_tile::Tile_Value const &v, char minzoom);

	AttributeSet() { }
	AttributeSet(const AttributeSet &a) {
		// TODO: can we just use the default copy constructor?
		// This was needed to avoid copying the atomic<bool> which I am currently
		// discarding.
		hash_value = a.hash_value;
		values = a.values;
	}

private:
	// hash_value is memoized when AttributeStore tries to add it.
	// Storing this costs us 4 bytes, but we make that back via shenanigans
	// that require a fast hash_value computation.
	size_t hash_value;

// TODO: Chesterton's (memory?) fence... is this being used to impose
//   memory read barriers?
// Maybe it ought not be quietly discarded?
//	std::atomic<bool> lock_ = { false };
//	void lock() { while(lock_.exchange(true, std::memory_order_acquire)); }
//	void unlock() { lock_.store(false, std::memory_order_release); }
};

// AttributeStore is the store for all AttributeSets
struct AttributeStore {
	tsl::ordered_set<AttributeSet, AttributeSet::hash_function> attribute_sets;
	mutable std::mutex mutex;
	int lookups=0;

	AttributeIndex add(AttributeSet &attributes);
	std::set<AttributePair, AttributeSet::key_value_less> get(AttributeIndex index) const;
	void reportSize() const;
	void doneReading();
	
	AttributeStore() {
		// Initialise with an empty set at position 0
		AttributeSet blank;
		attribute_sets.insert(blank);
	}
};

#endif //_ATTRIBUTE_STORE_H
