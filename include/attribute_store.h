/*! \file */ 
#ifndef _ATTRIBUTE_STORE_H
#define _ATTRIBUTE_STORE_H

#include <mutex>
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
	static std::unique_ptr<AttributeKeyStoreImmutable> immutable;
	// NB: we use a deque, not a vector, because a deque never invalidates
	// pointers to its members as long as you only push_back
	static std::deque<std::string> keys;
};

#define AP_TYPE_FALSE 0
#define AP_TYPE_TRUE 1
#define AP_TYPE_FLOAT 2
#define AP_TYPE_STRING 3
// AttributePair is a key/value pair (with minzoom)
struct AttributePair {
	std::string stringValue;
	float floatValue;
	short keyIndex;
	char minzoom;
	char valueType;

	AttributePair(std::string const &key, bool value, char minzoom)
		: keyIndex(AttributeKeyStore::key2index(key)), valueType(value ? AP_TYPE_TRUE : AP_TYPE_FALSE), minzoom(minzoom)
	{
	}
	AttributePair(std::string const &key, const std::string& value, char minzoom)
		: keyIndex(AttributeKeyStore::key2index(key)), valueType(AP_TYPE_STRING), stringValue(value), minzoom(minzoom)
	{
	}
	AttributePair(std::string const &key, float value, char minzoom)
		: keyIndex(AttributeKeyStore::key2index(key)), valueType(AP_TYPE_FLOAT), floatValue(value), minzoom(minzoom)
	{
	}

	bool operator==(const AttributePair &other) const {
		if (minzoom!=other.minzoom || keyIndex!=other.keyIndex || valueType!=other.valueType) return false;
		if (valueType == AP_TYPE_STRING)
			return stringValue == other.stringValue;

		if (valueType == AP_TYPE_FLOAT)
			return floatValue == other.floatValue;

		return true;
	}

	bool has_string_value() const { return valueType == AP_TYPE_STRING; }
	bool has_float_value() const { return valueType == AP_TYPE_FLOAT; }
	bool has_bool_value() const { return valueType == AP_TYPE_TRUE || valueType == AP_TYPE_FALSE; };

	const std::string& string_value() const { return stringValue; }
	float float_value() const { return floatValue; }
	bool bool_value() const { return valueType == AP_TYPE_TRUE; }

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

		// Single digit integers are eligible.
		if (has_float_value()) {
			float v = float_value();

			if (v >= 0 && v <= 9 && (v == 0 || v == 1 || v == 2 || v == 3 || v == 4 || v == 5 || v == 6 || v == 7 || v == 8 || v == 9))
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
		//return pairs[shard][offset];
		return pairs[shard].at(offset);
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
			if (lhs->valueType < rhs->valueType) return true;
			if (lhs->valueType > rhs->valueType) return false;

			if (lhs->has_string_value()) return lhs->string_value() < rhs->string_value();
			if (lhs->has_bool_value()) return lhs->bool_value() < rhs->bool_value();
			if (lhs->has_float_value()) return lhs->float_value() < rhs->float_value();
			throw std::runtime_error("Invalid type in attribute store");
		}
	}; 

	static std::vector<std::deque<AttributePair>> pairs;

private:
	// We refer to all attribute pairs by index.
	//
	// Each shard is responsible for a portion of the key space.
	// 
	// The 0th shard is special: it's the hot shard, for pairs
	// we suspect will be popular. It only ever has 64KB items,
	// so that we can reference it with a short.
	static std::vector<std::mutex> pairsMutex;
	static std::vector<boost::container::flat_map<const AttributePair*, uint32_t, AttributePairStore::key_value_less_ptr>> pairsMaps;
};

// AttributeSet is a set of AttributePairs
// = the complete attributes for one object
struct AttributeSet {

	struct hash_function {
		size_t operator()(const AttributeSet &attributes) const {
			// Values are in canonical form after finalize_set is called, so
			// can hash them in the order they're stored.
			size_t idx = attributes.values.size();
			for (auto const &i: attributes.values)
				boost::hash_combine(idx, i);

			return idx;

		}
	};
	bool operator==(const AttributeSet &other) const {
		if (values.size() != other.values.size())
			return false;

		// Equivalent if, for every value in values, there is a value in other.values
		// whose pair is the same.
		//
		// NB: finalize_set ensures values are in canonical order, so we can just
		// do a pairwise comparison.

		for (size_t i = 0; i < values.size(); i++)
			if (values[i] != other.values[i])
				return false;

		return true;
	}

	void finalize_set();

	std::vector<uint32_t> values;

	void add(std::string const &key, const std::string& v, char minzoom);
	void add(std::string const &key, float v, char minzoom);
	void add(std::string const &key, bool v, char minzoom);

	AttributeSet() { }
	AttributeSet(const AttributeSet &a) {
		// TODO: can we just use the default copy constructor?
		// This was needed to avoid copying the atomic<bool> which I am currently
		// discarding.
		values = a.values;
	}

private:
	void add(AttributePair const &kv);

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
