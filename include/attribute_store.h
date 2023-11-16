/*! \file */ 
#ifndef _ATTRIBUTE_STORE_H
#define _ATTRIBUTE_STORE_H

#include <mutex>
#include <iostream>
#include <atomic>
#include <boost/functional/hash.hpp>
#include <boost/container/flat_map.hpp>
#include <vector>
#include <unordered_map>
#include <tsl/ordered_set.h>

// TODO: the PairStore and KeyStore have static scope. Should probably
// do the work to move them into AttributeStore, and change how
// AttributeSet is interacted with?
//
// OTOH, AttributeStore's lifetime is the process's lifetime, so it'd
// just be a good coding style thing, not actually preventing a leak.

/* AttributeStore - global dictionary for attributes */

typedef uint32_t AttributeIndex; // check this is enough

class AttributeKeyStore {
public:
	// We jump through some hoops to have no locks for most readers,
	// locking only if we need to add the value.
	static uint16_t key2index(const std::string& key) {
		std::lock_guard<std::mutex> lock(keys2index_mutex);
		const auto& rv = keys2index.find(key);

		if (rv != keys2index.end())
			return rv->second;

		// 0 is used as a sentinel, so ensure that the 0th element is just a dummy element.
		if (keys.size() == 0)
			keys.push_back("");

		uint16_t newIndex = keys.size();

		// This is very unlikely. We expect more like 50-100 keys.
		if (newIndex >= 65535)
			throw std::out_of_range("more than 65,536 unique keys");

		keys2index[key] = newIndex;
		keys.push_back(key);
		return newIndex;
	}

	static const std::string& getKey(uint16_t index) {
		// TODO: is this safe? I suspect it might be in practice because
		// we only have ~50 keys, which is smaller than the initial size of
		// a deque. But I think if the deque ever grew concurrently
		// with a read, we'd be in trouble.
		//
		// We should either wrap this in a futex, or access it through
		// the immutable key score.
		std::lock_guard<std::mutex> lock(keys2index_mutex);
		return keys[index];
	}

private:
	static std::mutex keys2index_mutex;
	// NB: we use a deque, not a vector, because a deque never invalidates
	// pointers to its members as long as you only push_back
	static std::deque<std::string> keys;
	static std::map<const std::string, uint16_t> keys2index;
};

enum class AttributePairType: char { False = 0, True = 1, Float = 2, String = 3 };
// AttributePair is a key/value pair (with minzoom)
struct AttributePair {
	std::string stringValue;
	float floatValue;
	short keyIndex;
	char minzoom;
	AttributePairType valueType;

	AttributePair(std::string const &key, bool value, char minzoom)
		: keyIndex(AttributeKeyStore::key2index(key)), valueType(value ? AttributePairType::True : AttributePairType::False), minzoom(minzoom)
	{
	}
	AttributePair(std::string const &key, const std::string& value, char minzoom)
		: keyIndex(AttributeKeyStore::key2index(key)), valueType(AttributePairType::String), stringValue(value), minzoom(minzoom)
	{
	}
	AttributePair(std::string const &key, float value, char minzoom)
		: keyIndex(AttributeKeyStore::key2index(key)), valueType(AttributePairType::Float), floatValue(value), minzoom(minzoom)
	{
	}

	bool operator==(const AttributePair &other) const {
		if (minzoom!=other.minzoom || keyIndex!=other.keyIndex || valueType!=other.valueType) return false;
		if (valueType == AttributePairType::String)
			return stringValue == other.stringValue;

		if (valueType == AttributePairType::Float)
			return floatValue == other.floatValue;

		return true;
	}

	bool has_string_value() const { return valueType == AttributePairType::String; }
	bool has_float_value() const { return valueType == AttributePairType::Float; }
	bool has_bool_value() const { return valueType == AttributePairType::True || valueType == AttributePairType::False; };

	const std::string& string_value() const { return stringValue; }
	float float_value() const { return floatValue; }
	bool bool_value() const { return valueType == AttributePairType::True; }

	bool hot() const {
		// Is this pair a candidate for the hot pool?

		// Hot pairs are pairs that we think are likely to be re-used, like
		// tunnel=0, highway=yes, and so on.
		//
		// The trick is that we commit to putting them in the hot pool
		// before we know if we were right.

		// All boolean pairs are eligible.
		if (has_bool_value())
			return true;

		// Small integers are eligible.
		if (has_float_value()) {
			float v = float_value();

			if (ceil(v) == v && v >= 0 && v <= 25)
				return true;
		}

		// The remaining things should be strings, but just in case...
		if (!has_string_value())
			return false;

		// Only strings that are IDish are eligible: only lowercase letters.
		bool ok = true;
		for (const auto& c: string_value()) {
			if (c != '-' && c != '_' && (c < 'a' || c > 'z'))
				return false;
		}

		// Keys that sound like name, name:en, etc, aren't eligible.
		const auto& keyName = AttributeKeyStore::getKey(keyIndex);
		if (keyName.size() >= 4 && keyName[0] == 'n' && keyName[1] == 'a' && keyName[2] == 'm' && keyName[3])
			return false;

		return true;
	}

	const std::string& key() const {
		return AttributeKeyStore::getKey(keyIndex);
	}

	size_t hash() const {
		std::size_t rv = minzoom;
		boost::hash_combine(rv, keyIndex);
		boost::hash_combine(rv, valueType);

		if(has_string_value())
			boost::hash_combine(rv, string_value());
		else if(has_float_value())
			boost::hash_combine(rv, float_value());
		else if(has_bool_value())
			boost::hash_combine(rv, bool_value());
		else {
			throw new std::out_of_range("cannot hash pair, unknown value");
		}

		return rv;
	}
};


// We shard the cold pools to reduce the odds of lock contention on
// inserting/retrieving the "cold" pairs.
//
// It should be at least 2x the number of your cores -- 256 shards is probably
// reasonable for most people.
//
// We also reserve the bottom shard for the hot pool. Since a shard is 16M entries,
// but the hot pool is only 64KB entries, we're wasting a little bit of key space.
#define SHARD_BITS 8
#define PAIR_SHARDS (1 << SHARD_BITS)

class AttributePairStore {
public:
	static const AttributePair& getPair(uint32_t i) {
		uint32_t shard = i >> (32 - SHARD_BITS);
		uint32_t offset = i & (~(~0u << (32 - SHARD_BITS)));

		std::lock_guard<std::mutex> lock(pairsMutex[shard]);
		return pairs[shard][offset];
		//return pairs[shard].at(offset);
	};

	static uint32_t addPair(const AttributePair& pair);

	struct key_value_less {
		bool operator()(AttributePair const &lhs, AttributePair const& rhs) const {
			if (lhs.minzoom != rhs.minzoom)
				return lhs.minzoom < rhs.minzoom;
			if (lhs.keyIndex != rhs.keyIndex)
				return lhs.keyIndex < rhs.keyIndex;
			if (lhs.valueType < rhs.valueType) return true;
			if (lhs.valueType > rhs.valueType) return false;

			if (lhs.has_string_value()) return lhs.string_value() < rhs.string_value();
			if (lhs.has_bool_value()) return lhs.bool_value() < rhs.bool_value();
			if (lhs.has_float_value()) return lhs.float_value() < rhs.float_value();
			throw std::runtime_error("Invalid type in attribute store");
		}
	}; 

	struct key_value_less_ptr {
		bool operator()(AttributePair const* lhs, AttributePair const* rhs) const {            
			if (lhs->minzoom != rhs->minzoom)
				return lhs->minzoom < rhs->minzoom;
			if (lhs->keyIndex != rhs->keyIndex)
				return lhs->keyIndex < rhs->keyIndex;
			if (lhs->valueType != rhs->valueType) return lhs->valueType < rhs->valueType;

			if (lhs->has_string_value()) return lhs->string_value() < rhs->string_value();
			if (lhs->has_bool_value()) return lhs->bool_value() < rhs->bool_value();
			if (lhs->has_float_value()) return lhs->float_value() < rhs->float_value();
			throw std::runtime_error("Invalid type in attribute store");
		}
	}; 

	static std::vector<std::deque<AttributePair>> pairs;
	static std::vector<boost::container::flat_map<const AttributePair*, uint32_t, AttributePairStore::key_value_less_ptr>> pairsMaps;

private:
	// We refer to all attribute pairs by index.
	//
	// Each shard is responsible for a portion of the key space.
	// 
	// The 0th shard is special: it's the hot shard, for pairs
	// we suspect will be popular. It only ever has 64KB items,
	// so that we can reference it with a short.
	static std::vector<std::mutex> pairsMutex;
};

// AttributeSet is a set of AttributePairs
// = the complete attributes for one object
struct AttributeSet {

	struct hash_function {
		size_t operator()(const AttributeSet &attributes) const {
			// Values are in canonical form after finalize_set is called, so
			// can hash them in the order they're stored.
			if (attributes.useVector) {
				const size_t n = attributes.intValues.size();
				size_t idx = n;
				for (int i = 0; i < n; i++)
					boost::hash_combine(idx, attributes.intValues[i]);

				return idx;
			}

			size_t idx = 0;
			for (int i = 0; i < 12; i++)
				boost::hash_combine(idx, attributes.shortValues[i]);

			return idx;
		}
	};
	bool operator==(const AttributeSet &other) const {
		// Equivalent if, for every value in values, there is a value in other.values
		// whose pair is the same.
		//
		// NB: finalize_set ensures values are in canonical order, so we can just
		// do a pairwise comparison.

		if (useVector != other.useVector)
			return false;

		if (useVector) {
			const size_t n = intValues.size();
			const size_t otherN = other.intValues.size();
			if (n != otherN)
				return false;
			for (size_t i = 0; i < n; i++)
				if (intValues[i] != other.intValues[i])
					return false;

			return true;
		}

		return memcmp(shortValues, other.shortValues, sizeof(shortValues)) == 0;
	}

	void finalize_set();

	// We store references to AttributePairs either in an array of shorts
	// or a vector of 32-bit ints.
	//
	// The array of shorts is not _really_ an array of shorts. It's meant
	// to be interpreted as 4 shorts, and then 4 ints.
	bool useVector;
	union {
		short shortValues[12];
		std::vector<uint32_t> intValues;
	};

	size_t num_pairs() const {
		if (useVector)
			return intValues.size();

		size_t rv = 0;
		for (int i = 0; i < 8; i++)
			if (is_set(i))
				rv++;

		return rv;
	}

	const uint32_t get_pair(size_t i) const {
		if (useVector)
			return intValues[i];

		size_t j = 0;
		size_t actualIndex = 0;
		// Advance actualIndex to the first non-zero entry, e.g. if
		// the first thing added has a 4-byte index, our first entry
		// is at location 4, not 0.
		while(!is_set(actualIndex)) actualIndex++;

		while (j < i) {
			j++;
			actualIndex++;
			while(!is_set(actualIndex)) actualIndex++;
		}

		return get_value_at_index(actualIndex);
	}

	void add(std::string const &key, const std::string& v, char minzoom);
	void add(std::string const &key, float v, char minzoom);
	void add(std::string const &key, bool v, char minzoom);

	AttributeSet(): useVector(false), shortValues({}) {}
	AttributeSet(const AttributeSet &&a) = delete;

	AttributeSet(const AttributeSet &a) {
		useVector = a.useVector;

		if (useVector) {
			new (&intValues) std::vector<uint32_t>;
			intValues = a.intValues;
			for (int i = 0; i < 12; i++)
				shortValues[i] = 0;
		} else {
			for (int i = 0; i < 12; i++)
				shortValues[i] = a.shortValues[i];
		}
	}

	~AttributeSet() {
		if (useVector)
			intValues.~vector();
	}

private:
	void add(AttributePair const &kv);
	void add(uint32_t index);
	void set_value_at_index(size_t index, uint32_t value) {
		if (useVector) {
			throw std::out_of_range("set_value_at_index called for useVector=true");
		}

		if (index < 4 && value < (1 << 16)) {
			shortValues[index] = (uint16_t)value;
		} else if (index >= 4 && index < 8) {
			((uint32_t*)(&shortValues[4]))[index - 4] = value;
		} else {
			throw std::out_of_range("set_value_at_index out of bounds");
		}
	}
	uint32_t get_value_at_index(size_t index) const {
		if (index < 4)
			return shortValues[index];

		return ((uint32_t*)(&shortValues[4]))[index - 4];
	}
	bool is_set(size_t index) const {
		if (index < 4) return shortValues[index] != 0;

		const size_t newIndex = 4 + 2 * (index - 4);
		return shortValues[newIndex] != 0 || shortValues[newIndex + 1] != 0;
	}
};

// AttributeStore is the store for all AttributeSets
struct AttributeStore {
	tsl::ordered_set<AttributeSet, AttributeSet::hash_function> attribute_sets;
	mutable std::mutex mutex;
	int lookups=0;

	AttributeIndex add(AttributeSet &attributes);
	std::set<AttributePair, AttributePairStore::key_value_less> get(AttributeIndex index) const;
	void reportSize() const;
	void doneReading();
	
	AttributeStore() {
		// Initialise with an empty set at position 0
		AttributeSet blank;
		attribute_sets.insert(blank);
	}
};

#endif //_ATTRIBUTE_STORE_H
