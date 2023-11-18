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

/* AttributeStore - global dictionary for attributes */

typedef uint32_t AttributeIndex; // check this is enough

struct string_ptr_less_than {
	bool operator()(const std::string* lhs, const std::string* rhs) const {            
		return *lhs < *rhs;
	}
}; 

class AttributeKeyStore {
public:
	uint16_t key2index(const std::string& key);
	const std::string& getKey(uint16_t index) const;
	std::atomic<uint32_t> keys2indexSize;

private:
	mutable std::mutex keys2indexMutex;
	// NB: we use a deque, not a vector, because a deque never invalidates
	// pointers to its members as long as you only push_back
	std::deque<std::string> keys;
	std::map<const std::string*, uint16_t, string_ptr_less_than> keys2index;
};

enum class AttributePairType: char { False = 0, True = 1, Float = 2, String = 3 };
// AttributePair is a key/value pair (with minzoom)
struct AttributePair {
	std::string stringValue_;
	float floatValue_;
	short keyIndex;
	char minzoom;
	AttributePairType valueType;

	AttributePair(uint32_t keyIndex, bool value, char minzoom)
		: keyIndex(keyIndex), valueType(value ? AttributePairType::True : AttributePairType::False), minzoom(minzoom)
	{
	}
	AttributePair(uint32_t keyIndex, const std::string& value, char minzoom)
		: keyIndex(keyIndex), valueType(AttributePairType::String), stringValue_(value), minzoom(minzoom)
	{
	}
	AttributePair(uint32_t keyIndex, float value, char minzoom)
		: keyIndex(keyIndex), valueType(AttributePairType::Float), floatValue_(value), minzoom(minzoom)
	{
	}

	bool operator==(const AttributePair &other) const {
		if (minzoom!=other.minzoom || keyIndex!=other.keyIndex || valueType!=other.valueType) return false;
		if (valueType == AttributePairType::String)
			return stringValue_ == other.stringValue_;

		if (valueType == AttributePairType::Float)
			return floatValue_ == other.floatValue_;

		return true;
	}

	bool hasStringValue() const { return valueType == AttributePairType::String; }
	bool hasFloatValue() const { return valueType == AttributePairType::Float; }
	bool hasBoolValue() const { return valueType == AttributePairType::True || valueType == AttributePairType::False; };

	const std::string& stringValue() const { return stringValue_; }
	float floatValue() const { return floatValue_; }
	bool boolValue() const { return valueType == AttributePairType::True; }

	static bool isHot(const AttributePair& pair, const std::string& keyName) {
		// Is this pair a candidate for the hot pool?

		// Hot pairs are pairs that we think are likely to be re-used, like
		// tunnel=0, highway=yes, and so on.
		//
		// The trick is that we commit to putting them in the hot pool
		// before we know if we were right.

		// All boolean pairs are eligible.
		if (pair.hasBoolValue())
			return true;

		// Small integers are eligible.
		if (pair.hasFloatValue()) {
			float v = pair.floatValue();

			if (ceil(v) == v && v >= 0 && v <= 25)
				return true;
		}

		// The remaining things should be strings, but just in case...
		if (!pair.hasStringValue())
			return false;

		// Only strings that are IDish are eligible: only lowercase letters.
		bool ok = true;
		for (const auto& c: pair.stringValue()) {
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

		if(hasStringValue())
			boost::hash_combine(rv, stringValue());
		else if(hasFloatValue())
			boost::hash_combine(rv, floatValue());
		else if(hasBoolValue())
			boost::hash_combine(rv, boolValue());
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
#define ATTRIBUTE_SHARDS (1 << SHARD_BITS)

class AttributePairStore {
public:
	AttributePairStore():
		pairs(ATTRIBUTE_SHARDS),
		pairsMaps(ATTRIBUTE_SHARDS),
		pairsMutex(ATTRIBUTE_SHARDS) {
	}

	const AttributePair& getPair(uint32_t i) const {
		uint32_t shard = i >> (32 - SHARD_BITS);
		uint32_t offset = i & (~(~0u << (32 - SHARD_BITS)));

		std::lock_guard<std::mutex> lock(pairsMutex[shard]);
		//return pairs[shard][offset];
		return pairs[shard].at(offset);
	};

	uint32_t addPair(const AttributePair& pair, bool isHot);

	struct key_value_less_ptr {
		bool operator()(AttributePair const* lhs, AttributePair const* rhs) const {            
			if (lhs->minzoom != rhs->minzoom)
				return lhs->minzoom < rhs->minzoom;
			if (lhs->keyIndex != rhs->keyIndex)
				return lhs->keyIndex < rhs->keyIndex;
			if (lhs->valueType != rhs->valueType) return lhs->valueType < rhs->valueType;

			if (lhs->hasStringValue()) return lhs->stringValue() < rhs->stringValue();
			if (lhs->hasBoolValue()) return lhs->boolValue() < rhs->boolValue();
			if (lhs->hasFloatValue()) return lhs->floatValue() < rhs->floatValue();
			throw std::runtime_error("Invalid type in attribute store");
		}
	}; 

	std::vector<std::deque<AttributePair>> pairs;
	std::vector<boost::container::flat_map<const AttributePair*, uint32_t, AttributePairStore::key_value_less_ptr>> pairsMaps;

private:
	// We refer to all attribute pairs by index.
	//
	// Each shard is responsible for a portion of the key space.
	// 
	// The 0th shard is special: it's the hot shard, for pairs
	// we suspect will be popular. It only ever has 64KB items,
	// so that we can reference it with a short.
	mutable std::vector<std::mutex> pairsMutex;
};

// AttributeSet is a set of AttributePairs
// = the complete attributes for one object
struct AttributeSet {

	struct less_ptr {
		bool operator()(const AttributeSet* lhs, const AttributeSet* rhs) const {            
			if (lhs->useVector != rhs->useVector)
				return lhs->useVector < rhs->useVector;

			if (lhs->useVector) {
				if (lhs->intValues.size() != rhs->intValues.size())
					return lhs->intValues.size() < rhs->intValues.size();

				for (int i = 0; i < lhs->intValues.size(); i++) {
					if (lhs->intValues[i] != rhs->intValues[i]) {
						return lhs->intValues[i] < rhs->intValues[i];
					}
				}

				return false;
			}

			for (int i = 0; i < sizeof(lhs->shortValues)/sizeof(lhs->shortValues[0]); i++) {
				if (lhs->shortValues[i] != rhs->shortValues[i]) {
					return lhs->shortValues[i] < rhs->shortValues[i];
				}
			}

			return false;
		}
	}; 

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

	void finalizeSet();

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
			for (int i = 0; i < sizeof(shortValues)/sizeof(shortValues[0]); i++)
				shortValues[i] = 0;
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
	std::vector<std::deque<AttributeSet>> sets;
	std::vector<boost::container::flat_map<const AttributeSet*, uint32_t, AttributeSet::less_ptr>> setsMaps;
	mutable std::vector<std::mutex> setsMutex;

	AttributeKeyStore keyStore;
	AttributePairStore pairStore;
	mutable std::mutex mutex;
	int lookups=0;

	AttributeIndex add(AttributeSet &attributes);
	std::vector<const AttributePair*> get(AttributeIndex index) const;
	void reportSize() const;
	void doneReading();

	void addAttribute(AttributeSet& attributeSet, std::string const &key, const std::string& v, char minzoom);
	void addAttribute(AttributeSet& attributeSet, std::string const &key, float v, char minzoom);
	void addAttribute(AttributeSet& attributeSet, std::string const &key, bool v, char minzoom);
	
	AttributeStore():
		sets(ATTRIBUTE_SHARDS),
		setsMaps(ATTRIBUTE_SHARDS),
		setsMutex(ATTRIBUTE_SHARDS) {
	}
};

#endif //_ATTRIBUTE_STORE_H
