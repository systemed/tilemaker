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

/* AttributeStore - global dictionary for attributes */

typedef uint32_t AttributeIndex; // check this is enough

// AttributePair is a key/value pair (with minzoom)
struct AttributePair {
	vector_tile::Tile_Value value;
	short keyIndex;
	char minzoom;

	AttributePair(std::string const &key, vector_tile::Tile_Value const &value, char minzoom)
		: keyIndex(key2index(key)), value(value), minzoom(minzoom)
	{ }

	bool operator==(const AttributePair &other) const {
		if (minzoom!=other.minzoom || keyIndex!=other.keyIndex) return false;
		if (value.has_string_value()) return other.value.has_string_value() && other.value.string_value()==value.string_value();
		if (value.has_bool_value())   return other.value.has_bool_value()   && other.value.bool_value()  ==value.bool_value();
		if (value.has_float_value())  return other.value.has_float_value()  && other.value.float_value() ==value.float_value();
		throw std::runtime_error("Invalid type in attribute store");
	}

	const std::string& key() const {
		// To be thread-safe, you must only read the actual string value after all
		// attribute sets have been constructed, e.g. when writing output values.
		//
		// In general, prefer to use keyIndex for equality checking when building
		// up the sets of AttributePairs.
		return keys[keyIndex];
	}

private:
	static std::map<std::string, uint16_t> keys2index;
	static std::vector<std::string> keys;
	static std::mutex rw_mutex;

	// TODO: the set of unique keys is relatively stable, like ~50 or so.
	//
	// Forcing all readers to take a lock might be expensive. Currently,
	// other locks dominate processing. Once PR #578 lands, revisit to see
	// if this is a bottleneck.
	//
	// If it is, re-write to have lock-free reads: publish a pointer to
	// an immutable map/vector for lookups.
	//
	// If a reader fails to find the key it wants, it could then grab a lock,
	// clone the map/vector, check again for its entry, insert if missing,
	// and swap the pointer to the immutable map/vector.
	static uint16_t key2index(const std::string& key) {
		std::lock_guard<std::mutex> lock(rw_mutex);

		try {
			return keys2index.at(key);
		} catch (std::out_of_range &err) {
			uint32_t newIndex = keys.size();
			if (newIndex > 65535)
				throw std::out_of_range("more than 65,536 unique keys");
			keys2index[key] = newIndex;
			keys.push_back(key);
			return newIndex;
		}
	}
};

// AttributeSet is a set of AttributePairs
// = the complete attributes for one object
struct AttributeSet {
	static bool compare(vector_tile::Tile_Value const &lhs, vector_tile::Tile_Value const &rhs) {
		auto lhs_id = type_index(lhs);
		auto rhs_id = type_index(lhs);
		if(lhs_id < rhs_id) return true;
		if(lhs_id > rhs_id) return false;
		switch(lhs_id) {
			case Index::BOOL:    return lhs.bool_value() < rhs.bool_value();
			case Index::FLOAT:   return lhs.float_value() < rhs.float_value();
			case Index::STRING:  return lhs.string_value() < rhs.string_value();
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

	enum class Index { BOOL, FLOAT, STRING };
	static Index type_index(vector_tile::Tile_Value const &v) {
		if     (v.has_string_value()) return Index::STRING;
		else if(v.has_float_value())  return Index::FLOAT;
		else                          return Index::BOOL;
	}

	struct hash_function {
		size_t operator()(const AttributeSet &attributes) const {
			auto idx = attributes.values.size();
			for(auto const &i: attributes.values) {
				boost::hash_combine(idx, i.minzoom);
				boost::hash_combine(idx, i.keyIndex);
				boost::hash_combine(idx, type_index(i.value));

				if(i.value.has_string_value())
					boost::hash_combine(idx, i.value.string_value());
				else if(i.value.has_float_value())
					boost::hash_combine(idx, i.value.float_value());
				else
					boost::hash_combine(idx, i.value.bool_value());
			}
			return idx;
		}
	};
	bool operator==(const AttributeSet &other) const {
		return values==other.values;
	}

	std::set<AttributePair, key_value_less> values;

	void add(AttributePair const &kv);
	void add(std::string const &key, vector_tile::Tile_Value const &v, char minzoom);

	AttributeSet() { }
	AttributeSet(const AttributeSet &a) { values = a.values; } // copy constructor, don't copy lock

private:
	std::atomic<bool> lock_ = { false };
	void lock() { while(lock_.exchange(true, std::memory_order_acquire)); }
	void unlock() { lock_.store(false, std::memory_order_release); }
};

// AttributeStore is the store for all AttributeSets
struct AttributeStore {
	tsl::ordered_set<AttributeSet, AttributeSet::hash_function> attribute_sets;
	mutable std::mutex mutex;
	int lookups=0;

	AttributeIndex add(AttributeSet const &attributes);
	const std::set<AttributePair, AttributeSet::key_value_less>& get(AttributeIndex index) const;
	void reportSize() const;
	void doneReading();
	
	AttributeStore() {
		// Initialise with an empty set at position 0
		AttributeSet blank;
		attribute_sets.insert(blank);
	}
};

#endif //_ATTRIBUTE_STORE_H
