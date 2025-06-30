#include "attribute_store.h"

#include <iostream>
#include <algorithm>

// AttributeKeyStore
thread_local std::map<const std::string*, uint16_t, string_ptr_less_than> tlsKeys2Index;
thread_local uint16_t tlsKeys2IndexSize = 0;
uint16_t AttributeKeyStore::key2index(const std::string& key) {
	// First, try to find the key in our thread-local copy.
	{
		const auto& rv = tlsKeys2Index.find(&key);
		if (rv != tlsKeys2Index.end())
			return rv->second;
	}

	// Not found, ensure our local map is up-to-date for future calls,
	// and fall through to the main map.
	//
	// Note that we can read `keys` without a lock
	while (tlsKeys2IndexSize < keys2indexSize) {
		tlsKeys2IndexSize++;
		tlsKeys2Index[&keys[tlsKeys2IndexSize]] = tlsKeys2IndexSize;
	}
	std::lock_guard<std::mutex> lock(keys2indexMutex);
	const auto& rv = keys2index.find(&key);

	if (rv != keys2index.end())
		return rv->second;

	// 0 is used as a sentinel, so ensure that the 0th element is just a dummy element.
	if (keys.size() == 0)
		keys.push_back("");

	uint16_t newIndex = keys.size();

	// This is very unlikely. We expect more like 50-100 keys.
	if (newIndex >= 512)
		throw std::out_of_range("more than 512 unique keys");

	keys2indexSize++;
	keys.push_back(key);
	keys2index[&keys[newIndex]] = newIndex;
	return newIndex;
}

const std::string& AttributeKeyStore::getKey(uint16_t index) const {
	std::lock_guard<std::mutex> lock(keys2indexMutex);
	return keys[index];
}

const std::string& AttributeKeyStore::getKeyUnsafe(uint16_t index) const {
	// NB: This is unsafe if called before the PBF has been fully read.
	// If called during the output phase, it's safe.
	return keys[index];
}

// AttributePair
void AttributePair::ensureStringIsOwned() {
	// Before we store an AttributePair in our long-term storage, we need
	// to make sure it's not pointing to a non-long-lived std::string.
	if (valueType != AttributePairType::String) return;

	stringValue_.ensureStringIsOwned();
}

// AttributePairStore
thread_local DequeMap<AttributePair> tlsHotShard(1 << 16);
const AttributePair& AttributePairStore::getPair(uint32_t i) const {
	uint32_t shard = i >> (32 - SHARD_BITS);
	uint32_t offset = i & (~(~0u << (32 - SHARD_BITS)));

	if (shard == 0) {
		if (offset < tlsHotShard.size())
			return tlsHotShard[offset];

		{
			std::lock_guard<std::mutex> lock(pairsMutex[0]);
			tlsHotShard = pairs[0];
		}

		return tlsHotShard[offset];
	}

	std::lock_guard<std::mutex> lock(pairsMutex[shard]);
	return pairs[shard][offset];
};

const AttributePair& AttributePairStore::getPairUnsafe(uint32_t i) const {
	// NB: This is unsafe if called before the PBF has been fully read.
	// If called during the output phase, it's safe.

	uint32_t shard = i >> (32 - SHARD_BITS);
	uint32_t offset = i & (~(~0u << (32 - SHARD_BITS)));

	return pairs[shard][offset];
};

// Remember recently queried/added pairs so that we can return them in the
// future without taking a lock.
thread_local uint64_t tlsPairLookups = 0;
thread_local uint64_t tlsPairLookupsUncached = 0;

thread_local std::vector<const AttributePair*> cachedAttributePairPointers(64);
thread_local std::vector<uint32_t> cachedAttributePairIndexes(64);
uint32_t AttributePairStore::addPair(AttributePair& pair, bool isHot) {
	if (isHot) {
		{
			// First, check our thread-local map.
			const auto& index = tlsHotShard.find(pair);
			if (index != -1)
				return index;
		}

		// Not found, ensure our local map is up-to-date for future calls,
		// and fall through to the main map.
		if (!tlsHotShard.full()) {
			std::lock_guard<std::mutex> lock(pairsMutex[0]);
			tlsHotShard = pairs[0];
		}

		// This might be a popular pair, worth re-using.
		// Have we already assigned it a hot ID?
		std::lock_guard<std::mutex> lock(pairsMutex[0]);
		const auto& index = pairs[0].find(pair);
		if (index != -1)
			return index;

		if (!pairs[0].full()) {
			pair.ensureStringIsOwned();
			uint32_t offset = pairs[0].add(pair);
			uint32_t rv = (0 << (32 - SHARD_BITS)) + offset;
			return rv;
		}
	}

	// This is either not a hot key, or there's no room for in the hot shard.
	// Throw it on the pile with the rest of the pairs.
	size_t hash = pair.hash();

	const size_t candidateIndex = hash % cachedAttributePairPointers.size();
	// Before taking a lock, see if we've seen this attribute pair recently.

	tlsPairLookups++;
	if (tlsPairLookups % 1024 == 0) {
		lookups += 1024;
	}


	{
		const AttributePair* candidate = cachedAttributePairPointers[candidateIndex];

		if (candidate != nullptr && *candidate == pair)
			return cachedAttributePairIndexes[candidateIndex];
	}


	size_t shard = hash % ATTRIBUTE_SHARDS;
	// Shard 0 is for hot pairs -- pick another shard if it gets selected.
	if (shard == 0) shard = (hash >> 8) % ATTRIBUTE_SHARDS;
	if (shard == 0) shard = (hash >> 16) % ATTRIBUTE_SHARDS;
	if (shard == 0) shard = (hash >> 24) % ATTRIBUTE_SHARDS;
	if (shard == 0) shard = 1;

	std::lock_guard<std::mutex> lock(pairsMutex[shard]);

	tlsPairLookupsUncached++;
	if (tlsPairLookupsUncached % 1024 == 0)
		lookupsUncached += 1024;

	const auto& index = pairs[shard].find(pair);
	if (index != -1) {
		const uint32_t rv = (shard << (32 - SHARD_BITS)) + index;
		cachedAttributePairPointers[candidateIndex] = &pairs[shard][index];
		cachedAttributePairIndexes[candidateIndex] = rv;

		return rv;
	}

	pair.ensureStringIsOwned();
	uint32_t offset = pairs[shard].add(pair);

	if (offset >= (1 << (32 - SHARD_BITS)))
		throw std::out_of_range("pair shard overflow");

	uint32_t rv = (shard << (32 - SHARD_BITS)) + offset;
	return rv;
};


// AttributeSet
void AttributeSet::addPair(uint32_t pairIndex) {
	if (useVector) {
		intValues.push_back(pairIndex);
	} else {
		for (size_t i = (pairIndex < (1 << 16)) ? 0 : 4; i < 8; i++) {
			if (!isSet(i)) {
				setValueAtIndex(i, pairIndex);
				return;
			}
		}

		// Switch to a vector -- copy our existing values + add the new one.
		std::vector<uint32_t> tmp;
		for (int i = 0; i < numPairs(); i++) {
			tmp.push_back(getPair(i));
		}

		tmp.push_back(pairIndex);

		new (&intValues) std::vector<uint32_t>;
		useVector = true;
		intValues = tmp;
	}
}
void AttributeSet::removePairWithKey(const AttributePairStore& pairStore, uint32_t keyIndex) {
	// When adding a new key/value, we need to remove any existing pair that has that key.
	if (useVector) {
		for (int i = 0; i < intValues.size(); i++) {
			const AttributePair& p = pairStore.getPair(intValues[i]);
			if (p.keyIndex == keyIndex) {
				intValues.erase(intValues.begin() + i);
				return;
			}
		}

		return;
	}

	for (int i = 0; i < 8; i++) {
		const uint32_t pairIndex = getValueAtIndex(i);
		if (pairIndex != 0) {
			const AttributePair& p = pairStore.getPair(pairIndex);
			if (p.keyIndex == keyIndex) {
				setValueAtIndex(i, 0);
				return;
			}
		}
	}
}

void AttributeStore::addAttribute(AttributeSet& attributeSet, std::string const &key, const protozero::data_view v, char minzoom) {
	PooledString ps(&v);
	AttributePair kv(keyStore.key2index(key), ps, minzoom);
	bool isHot = AttributePair::isHot(key, v);
	attributeSet.addPair(pairStore.addPair(kv, isHot));
}
void AttributeStore::addAttribute(AttributeSet& attributeSet, std::string const &key, bool v, char minzoom) {
	AttributePair kv(keyStore.key2index(key),v,minzoom);
	bool isHot = true; // All bools are eligible to be hot pairs
	attributeSet.addPair(pairStore.addPair(kv, isHot));
}
void AttributeStore::addAttribute(AttributeSet& attributeSet, std::string const &key, double v, char minzoom) {
	AttributePair kv(keyStore.key2index(key),v,minzoom);
	bool isHot = v >= 0 && v <= 25 && ceil(v) == v; // Whole numbers in 0..25 are eligible to be hot pairs
	attributeSet.addPair(pairStore.addPair(kv, isHot));
}
void AttributeStore::addAttribute(AttributeSet& attributeSet, std::string const &key, int v, char minzoom) {
	AttributePair kv(keyStore.key2index(key),v,minzoom);
	bool isHot = v >= 0 && v <= 25;
	attributeSet.addPair(pairStore.addPair(kv, isHot));
}

void AttributeSet::finalize() {
	// Ensure that values are sorted, giving us a canonical representation,
	// so that we can have fast hash/equality functions.
	if (useVector) {
		sort(intValues.begin(), intValues.end());
	} else {
		uint32_t sortMe[8];
		sortMe[0] = shortValues[0];
		sortMe[1] = shortValues[1];
		sortMe[2] = shortValues[2];
		sortMe[3] = shortValues[3];
		uint32_t* intPtrs = (uint32_t*)(&shortValues[4]);
		sortMe[4] = intPtrs[0];
		sortMe[5] = intPtrs[1];
		sortMe[6] = intPtrs[2];
		sortMe[7] = intPtrs[3];
		shortValues[0] = 0;
		shortValues[1] = 0;
		shortValues[2] = 0;
		shortValues[3] = 0;
		shortValues[4] = 0;
		shortValues[5] = 0;
		shortValues[6] = 0;
		shortValues[7] = 0;
		shortValues[8] = 0;
		shortValues[9] = 0;
		shortValues[10] = 0;
		shortValues[11] = 0;
		std::sort(sortMe, &sortMe[8]);

		for (int i = 0; i < 8; i++)
			if (sortMe[i] != 0)
				addPair(sortMe[i]);
	}
}


// Remember recently queried/added sets so that we can return them in the
// future without taking a lock.
thread_local std::vector<const AttributeSet*> cachedAttributeSetPointers(64);
thread_local std::vector<AttributeIndex> cachedAttributeSetIndexes(64);

thread_local uint64_t tlsSetLookups = 0;
thread_local uint64_t tlsSetLookupsUncached = 0;
AttributeIndex AttributeStore::add(AttributeSet &attributes) {
	// TODO: there's probably a way to use C++ types to distinguish a finalized
	// and non-finalized AttributeSet, which would make this safer.
	attributes.finalize();

	size_t hash = attributes.hash();

	const size_t candidateIndex = hash % cachedAttributeSetPointers.size();
	// Before taking a lock, see if we've seen this attribute set recently.

	tlsSetLookups++;
	if (tlsSetLookups % 1024 == 0) {
		lookups += 1024;
	}


	{
		const AttributeSet* candidate = cachedAttributeSetPointers[candidateIndex];

		if (candidate != nullptr && *candidate == attributes)
			return cachedAttributeSetIndexes[candidateIndex];
	}

	size_t shard = hash % ATTRIBUTE_SHARDS;

	// We can't use the top 2 bits (see OutputObject's bitfields)
	shard = shard >> 2;

	std::lock_guard<std::mutex> lock(setsMutex[shard]);
	tlsSetLookupsUncached++;
	if (tlsSetLookupsUncached % 1024 == 0)
		lookupsUncached += 1024;

	const uint32_t offset = sets[shard].add(attributes);
	if (offset >= (1 << (32 - SHARD_BITS)))
		throw std::out_of_range("set shard overflow");

	uint32_t rv = (shard << (32 - SHARD_BITS)) + offset;

	cachedAttributeSetPointers[candidateIndex] = &sets[shard][offset];
	cachedAttributeSetIndexes[candidateIndex] = rv;
	return rv;
}

std::vector<const AttributePair*> AttributeStore::getUnsafe(AttributeIndex index) const {
	// NB: This is unsafe if called before the PBF has been fully read.
	// If called during the output phase, it's safe.

	try {
		uint32_t shard = index >> (32 - SHARD_BITS);
		uint32_t offset = index & (~(~0u << (32 - SHARD_BITS)));

		const AttributeSet& attrSet = sets[shard].at(offset);

		const size_t n = attrSet.numPairs();

		std::vector<const AttributePair*> rv;
		for (size_t i = 0; i < n; i++) {
			rv.push_back(&pairStore.getPairUnsafe(attrSet.getPair(i)));
		}

		return rv;
	} catch (std::out_of_range &err) {
		throw std::runtime_error("Failed to fetch attributes at index "+std::to_string(index));
	}
}

size_t AttributeStore::size() const {
	size_t numAttributeSets = 0;
	for (int i = 0; i < ATTRIBUTE_SHARDS; i++)
		numAttributeSets += sets[i].size();

	return numAttributeSets;
}

void AttributeStore::reportSize() const {
	std::cout << "Attributes: " << size() << " sets from " << lookups.load() << " objects (" << lookupsUncached.load() << " uncached), " << pairStore.lookups.load() << " pairs (" << pairStore.lookupsUncached.load() << " uncached)" << std::endl;

	// Print detailed histogram of frequencies of attributes.
	if (false) {
		for (int i = 0; i < ATTRIBUTE_SHARDS; i++) {
			std::cout << "pairs[" << i << "] has " << pairStore.pairs[i].size() << " entries" << std::endl;
		}

		std::map<uint32_t, uint32_t> tagCountDist;

		for (size_t i = 0; i < pairStore.pairs.size(); i++) {
			std::cout << "pairs[" << i << "] has size " << pairStore.pairs[i].size() << std::endl;
			size_t j = 0;
			for (const auto& ap: pairStore.pairs[i]) {
				std::cout << "pairs[" << i << "][" << j << "] keyIndex=" << ap.keyIndex << " minzoom=" << (65+ap.minzoom) << " stringValue=" << ap.stringValue() << " floatValue=" << ap.floatValue() << " boolValue=" << ap.boolValue() << " key=" << keyStore.getKey(ap.keyIndex) << std::endl;
				j++;

			}
		}
		size_t pairs = 0;
		std::map<uint32_t, size_t> uniques;
		for (const auto& attributeSetSet: sets) {
			for (const auto& attrSet: attributeSetSet) {
				pairs += attrSet.numPairs();

				try {
					tagCountDist[attrSet.numPairs()]++;
				} catch (std::out_of_range &err) {
					tagCountDist[attrSet.numPairs()] = 1;
				}

				const size_t n = attrSet.numPairs();
				for (size_t i = 0; i < n; i++) {
					uint32_t attr = attrSet.getPair(i);
					try {
						uniques[attr]++;
					} catch (std::out_of_range &err) {
						uniques[attr] = 1;
					}
				}
			}
		}
		std::cout << "AttributePairs: " << pairs << ", unique: " << uniques.size() << std::endl;

		for (const auto entry: tagCountDist) {
			std::cout << "tagCountDist " << entry.first << " tags occurs " << entry.second << " times" << std::endl;
		}

		for (const auto entry: uniques) {
			const auto& pair = pairStore.getPair(entry.first);
			// It's useful to occasionally confirm that anything with high freq has hot=1,
			// and also that things with hot=1 have high freq.
			std::cout << "attrpair freq= " << entry.second << " hot=" << (entry.first < 65536 ? 1 : 0) << " key=" << keyStore.getKey(pair.keyIndex) <<" stringValue=" << pair.stringValue() << " floatValue=" << pair.floatValue() << " boolValue=" << pair.boolValue() << std::endl;
		}
	}
}

void AttributeStore::reset() {
	// This is only used for tests.
	tlsKeys2Index.clear();
	tlsKeys2IndexSize = 0;

	tlsHotShard.clear();

	for (int i = 0; i < cachedAttributeSetPointers.size(); i++)
		cachedAttributeSetPointers[i] = nullptr;

	for (int i = 0; i < cachedAttributePairPointers.size(); i++)
		cachedAttributePairPointers[i] = nullptr;
}

void AttributeStore::finalize() {
	finalized = true;
	keyStore.finalize();
	pairStore.finalize();
}
