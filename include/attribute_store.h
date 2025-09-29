/*! \file */ 
#ifndef _ATTRIBUTE_STORE_H
#define _ATTRIBUTE_STORE_H

#include <mutex>
#include <deque>
#include <map>
#include <iostream>
#include <atomic>
#include <boost/functional/hash.hpp>
#include <boost/container/flat_map.hpp>
#include <vector>
#include <protozero/data_view.hpp>
#include "pooled_string.h"
#include "deque_map.h"

/* AttributeStore - global dictionary for attributes */

typedef uint32_t AttributeIndex; // check this is enough

struct string_ptr_less_than {
	bool operator()(const std::string* lhs, const std::string* rhs) const {            
		return *lhs < *rhs;
	}
}; 

class AttributeKeyStore {
public:
	AttributeKeyStore(): finalized(false), keys2indexSize(0) {}
	uint16_t key2index(const std::string& key);
	const std::string& getKey(uint16_t index) const;
	const std::string& getKeyUnsafe(uint16_t index) const;
	void finalize() { finalized = true; }
	std::atomic<uint32_t> keys2indexSize;

private:
	bool finalized;
	mutable std::mutex keys2indexMutex;
	// NB: we use a deque, not a vector, because a deque never invalidates
	// pointers to its members as long as you only push_back
	std::deque<std::string> keys;
	std::map<const std::string*, uint16_t, string_ptr_less_than> keys2index;
};

enum class AttributePairType: uint8_t { String = 0, Float = 1, Bool = 2, Int = 3 };
// AttributePair is a key/value pair (with minzoom)
#pragma pack(push, 1)
struct AttributePair {
	unsigned short keyIndex : 9;
	AttributePairType valueType : 2;
	uint8_t minzoom : 5; // Support zooms from 0..31. In practice, we expect z16 to be the biggest minzoom.
	union {
		double doubleValue_;
		PooledString stringValue_;
	};

	AttributePair(uint32_t keyIndex, bool value, char minzoom)
		: keyIndex(keyIndex), valueType(AttributePairType::Bool), minzoom(minzoom), doubleValue_(value ? 1 : 0)
	{
	}
	AttributePair(uint32_t keyIndex, const PooledString& value, char minzoom)
		: keyIndex(keyIndex), valueType(AttributePairType::String), stringValue_(value), minzoom(minzoom)
	{
	}
	AttributePair(uint32_t keyIndex, double value, char minzoom)
		: keyIndex(keyIndex), valueType(AttributePairType::Float), minzoom(minzoom), doubleValue_(value)
	{
	}
	AttributePair(uint32_t keyIndex, int value, char minzoom)
		: keyIndex(keyIndex), valueType(AttributePairType::Int), minzoom(minzoom), doubleValue_(static_cast<double>(value))
	{
	}

	AttributePair(const AttributePair& other):
		keyIndex(other.keyIndex), valueType(other.valueType), minzoom(other.minzoom)
	{
		if (valueType == AttributePairType::String) {
			stringValue_ = other.stringValue_;
			return;
		}
			
		doubleValue_ = other.doubleValue_;
	}

	AttributePair& operator=(const AttributePair& other) {
		keyIndex = other.keyIndex;
		valueType = other.valueType;
		minzoom = other.minzoom;

		if (valueType == AttributePairType::String) {
			stringValue_ = other.stringValue_;
			return *this;
		}

		doubleValue_ = other.doubleValue_;
		return *this;
	}

	bool operator<(const AttributePair& other) const {
		if (minzoom != other.minzoom)
			return minzoom < other.minzoom;
		if (keyIndex != other.keyIndex)
			return keyIndex < other.keyIndex;
		if (valueType != other.valueType) return valueType < other.valueType;

		if (hasStringValue()) return pooledString() < other.pooledString();
		return doubleValue_ < other.doubleValue_;
	}

	bool operator==(const AttributePair &other) const {
		if (minzoom!=other.minzoom || keyIndex!=other.keyIndex || valueType!=other.valueType) return false;
		if (valueType == AttributePairType::String) return stringValue_ == other.stringValue_;
		return doubleValue_ == other.doubleValue_;
	}

	bool hasStringValue() const { return valueType == AttributePairType::String; }
	bool hasFloatValue() const { return valueType == AttributePairType::Float; }
	bool hasBoolValue() const { return valueType == AttributePairType::Bool; }
	bool hasIntValue() const { return valueType == AttributePairType::Int; }

	const PooledString& pooledString() const { return stringValue_; }
	const std::string stringValue() const { return stringValue_.toString(); }
	float floatValue() const { return static_cast<float>(doubleValue_); }
	bool boolValue() const { return doubleValue_; }
	int intValue() const { return static_cast<int64_t>(doubleValue_); }

	void ensureStringIsOwned();

	static bool isHot(const std::string& keyName, const protozero::data_view value) {
		// Is this pair a candidate for the hot pool?

		// Hot pairs are pairs that we think are likely to be re-used, like
		// tunnel=0, highway=yes, and so on.
		//
		// The trick is that we commit to putting them in the hot pool
		// before we know if we were right.

		// The rules for floats/booleans are managed in their addAttribute call.

		// Only strings that are IDish are eligible: only lowercase letters.
		bool ok = true;
		for (size_t i = 0; i < value.size(); i++) {
			const auto c = value.data()[i];
			if (c != '-' && c != '_' && (c < 'a' || c > 'z'))
				return false;
		}

		// Keys that sound like name, name:en, etc, aren't eligible.
		if (keyName.size() >= 4 && keyName[0] == 'n' && keyName[1] == 'a' && keyName[2] == 'm' && keyName[3])
			return false;

		return true;
	}

	size_t hash() const {
		std::size_t rv = minzoom;
		boost::hash_combine(rv, keyIndex);
		boost::hash_combine(rv, valueType);

		if(hasStringValue()) {
			const char* data = pooledString().data();
			boost::hash_range(rv, data, data + pooledString().size());
		} else if(hasFloatValue())
			boost::hash_combine(rv, doubleValue_);
		else if(hasIntValue())
			boost::hash_combine(rv, doubleValue_);
		else if(hasBoolValue())
			boost::hash_combine(rv, boolValue());
		else {
			throw new std::out_of_range("cannot hash pair, unknown value");
		}

		return rv;
	}
};
#pragma pack(pop)


// We shard the cold pools to reduce the odds of lock contention on
// inserting/retrieving the "cold" pairs.
//
// It should be at least 2x the number of your cores -- 256 shards is probably
// reasonable for most people.
//
// We also reserve the bottom shard for the hot pool.
#define SHARD_BITS 14
#define ATTRIBUTE_SHARDS (1 << SHARD_BITS)

class AttributeStore;
class AttributePairStore {
public:
	AttributePairStore():
		finalized(false),
		pairsMutex(ATTRIBUTE_SHARDS),
		lookups(0),
		lookupsUncached(0)
	{
		// The "hot" shard has a capacity of 64K, the others are unbounded.
		pairs.push_back(DequeMap<AttributePair>(1 << 16));
		// Reserve offset 0 as a sentinel
		pairs[0].add(AttributePair(0, false, 0));
		for (size_t i = 1; i < ATTRIBUTE_SHARDS; i++)
			pairs.push_back(DequeMap<AttributePair>());
	}

	void finalize() { finalized = true; }
	const AttributePair& getPair(uint32_t i) const;
	const AttributePair& getPairUnsafe(uint32_t i) const;
	uint32_t addPair(AttributePair& pair, bool isHot);


private:
	friend class AttributeStore;
	std::vector<DequeMap<AttributePair>> pairs;
	bool finalized;
	// We refer to all attribute pairs by index.
	//
	// Each shard is responsible for a portion of the key space.
	// 
	// The 0th shard is special: it's the hot shard, for pairs
	// we suspect will be popular. It only ever has 64KB items,
	// so that we can reference it with a short.
	mutable std::vector<std::mutex> pairsMutex;
	std::atomic<uint64_t> lookupsUncached;
	std::atomic<uint64_t> lookups;
};

// AttributeSet is a set of AttributePairs
// = the complete attributes for one object
struct AttributeSet {

	bool operator<(const AttributeSet& other) const {
		if (useVector != other.useVector)
			return useVector < other.useVector;

		if (useVector) {
			if (intValues.size() != other.intValues.size())
				return intValues.size() < other.intValues.size();

			for (int i = 0; i < intValues.size(); i++) {
				if (intValues[i] != other.intValues[i]) {
					return intValues[i] < other.intValues[i];
				}
			}

			return false;
		}

		for (int i = 0; i < sizeof(shortValues)/sizeof(shortValues[0]); i++) {
			if (shortValues[i] != other.shortValues[i]) {
				return shortValues[i] < other.shortValues[i];
			}
		}

		return false;
	}

	size_t hash() const {
		// Values are in canonical form after finalizeSet is called, so
		// can hash them in the order they're stored.
		if (useVector) {
			const size_t n = intValues.size();
			size_t idx = n;
			for (int i = 0; i < n; i++)
				boost::hash_combine(idx, intValues[i]);

			return idx;
		}

		size_t idx = 0;
		for (int i = 0; i < sizeof(shortValues)/sizeof(shortValues[0]); i++)
			boost::hash_combine(idx, shortValues[i]);

		return idx;
	}

	bool operator!=(const AttributeSet& other) const { return !(*this == other); }
	bool operator==(const AttributeSet &other) const {
		// Equivalent if, for every value in values, there is a value in other.values
		// whose pair is the same.
		//
		// NB: finalizeSet ensures values are in canonical order, so we can just
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

	void finalize();

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

	size_t numPairs() const {
		if (useVector)
			return intValues.size();

		size_t rv = 0;
		for (int i = 0; i < 8; i++)
			if (isSet(i))
				rv++;

		return rv;
	}

	const uint32_t getPair(size_t i) const {
		if (useVector)
			return intValues[i];

		size_t j = 0;
		size_t actualIndex = 0;
		// Advance actualIndex to the first non-zero entry, e.g. if
		// the first thing added has a 4-byte index, our first entry
		// is at location 4, not 0.
		while(!isSet(actualIndex)) actualIndex++;

		while (j < i) {
			j++;
			actualIndex++;
			while(!isSet(actualIndex)) actualIndex++;
		}

		return getValueAtIndex(actualIndex);
	}

	AttributeSet(): useVector(false) {
		for (int i = 0; i < sizeof(shortValues)/sizeof(shortValues[0]); i++)
			shortValues[i] = 0;
	}
	AttributeSet(const AttributeSet &&a) = delete;

	AttributeSet(const AttributeSet &a) {
		useVector = a.useVector;

		if (useVector) {
			new (&intValues) std::vector<uint32_t>;
			intValues = a.intValues;
		} else {
			for (int i = 0; i < sizeof(shortValues)/sizeof(shortValues[0]); i++)
				shortValues[i] = a.shortValues[i];
		}
	}

	~AttributeSet() {
		if (useVector)
			intValues.~vector();
	}

	void addPair(uint32_t index);
	void removePairWithKey(const AttributePairStore& pairStore, uint32_t keyIndex);
private:
	void setValueAtIndex(size_t index, uint32_t value) {
		if (useVector) {
			throw std::out_of_range("setValueAtIndex called for useVector=true");
		}

		if (index < 4 && value < (1 << 16)) {
			shortValues[index] = (uint16_t)value;
		} else if (index >= 4 && index < 8) {
			((uint32_t*)(&shortValues[4]))[index - 4] = value;
		} else {
			throw std::out_of_range("setValueAtIndex out of bounds");
		}
	}
	uint32_t getValueAtIndex(size_t index) const {
		if (index < 4)
			return shortValues[index];

		return ((uint32_t*)(&shortValues[4]))[index - 4];
	}
	bool isSet(size_t index) const {
		if (index < 4) return shortValues[index] != 0;

		const size_t newIndex = 4 + 2 * (index - 4);
		return shortValues[newIndex] != 0 || shortValues[newIndex + 1] != 0;
	}
};

// AttributeStore is the store for all AttributeSets
struct AttributeStore {
	AttributeIndex add(AttributeSet &attributes);
	std::vector<const AttributePair*> getUnsafe(AttributeIndex index) const;
	void reset(); // used for testing
	size_t size() const;
	void reportSize() const;
	void finalize();

	void addAttribute(AttributeSet& attributeSet, std::string const &key, const protozero::data_view v, char minzoom);
	void addAttribute(AttributeSet& attributeSet, std::string const &key, double v, char minzoom);
	void addAttribute(AttributeSet& attributeSet, std::string const &key, int v, char minzoom);
	void addAttribute(AttributeSet& attributeSet, std::string const &key, bool v, char minzoom);
	
	AttributeStore():
		finalized(false),
		sets(ATTRIBUTE_SHARDS),
		setsMutex(ATTRIBUTE_SHARDS),
		lookups(0),
		lookupsUncached(0) {
	}

	AttributeKeyStore keyStore;
	AttributePairStore pairStore;

private:
	bool finalized;
	std::vector<DequeMap<AttributeSet>> sets;
	mutable std::vector<std::mutex> setsMutex;

	mutable std::mutex mutex;
	std::atomic<uint64_t> lookupsUncached;
	std::atomic<uint64_t> lookups;
};

#endif //_ATTRIBUTE_STORE_H
