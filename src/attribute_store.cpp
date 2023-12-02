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
	if (newIndex >= 65535)
		throw std::out_of_range("more than 65,536 unique keys");

	keys2indexSize++;
	keys.push_back(key);
	keys2index[&keys[newIndex]] = newIndex;
	return newIndex;
}

const std::string& AttributeKeyStore::getKey(uint16_t index) const {
	std::lock_guard<std::mutex> lock(keys2indexMutex);
	return keys[index];
}

// AttributePairStore
thread_local boost::container::flat_map<const AttributePair*, uint32_t, AttributePairStore::key_value_less_ptr> tlsHotShardMap;
thread_local uint16_t tlsHotShardSize = 0;
const AttributePair& AttributePairStore::getPair(uint32_t i) const {
	uint32_t shard = i >> (32 - SHARD_BITS);
	uint32_t offset = i & (~(~0u << (32 - SHARD_BITS)));

	if (shard == 0)
		return hotShard[offset];

	std::lock_guard<std::mutex> lock(pairsMutex[shard]);
	//return pairs[shard][offset];
	return pairs[shard].at(offset);
};

uint32_t AttributePairStore::addPair(const AttributePair& pair, bool isHot) {
	if (isHot) {
		{
			// First, check our thread-local map.
			const auto& it = tlsHotShardMap.find(&pair);
			if (it != tlsHotShardMap.end())
				return it->second;
		}
		// Not found, ensure our local map is up-to-date for future calls,
		// and fall through to the main map.
		//
		// Note that we can read `hotShard` without a lock
		while (tlsHotShardSize < hotShardSize.load()) {
			tlsHotShardSize++;
			tlsHotShardMap[&hotShard[tlsHotShardSize]] = tlsHotShardSize;
		}

		// This might be a popular pair, worth re-using.
		// Have we already assigned it a hot ID?
		std::lock_guard<std::mutex> lock(pairsMutex[0]);
		const auto& it = pairsMaps[0].find(&pair);
		if (it != pairsMaps[0].end())
			return it->second;

		if (hotShardSize.load() < 1 << 16) {
			hotShardSize++;
			uint32_t offset = hotShardSize.load();

			hotShard[offset] = pair;
			const AttributePair* ptr = &hotShard[offset];
			uint32_t rv = (0 << (32 - SHARD_BITS)) + offset;
			pairsMaps[0][ptr] = rv;
			return rv;
		}
	}

	// This is either not a hot key, or there's no room for in the hot shard.
	// Throw it on the pile with the rest of the pairs.
	size_t hash = pair.hash();

	size_t shard = hash % ATTRIBUTE_SHARDS;
	// Shard 0 is for hot pairs -- pick another shard if it gets selected.
	if (shard == 0) shard = (hash >> 8) % ATTRIBUTE_SHARDS;
	if (shard == 0) shard = (hash >> 16) % ATTRIBUTE_SHARDS;
	if (shard == 0) shard = (hash >> 24) % ATTRIBUTE_SHARDS;
	if (shard == 0) shard = 1;

	std::lock_guard<std::mutex> lock(pairsMutex[shard]);
	const auto& it = pairsMaps[shard].find(&pair);
	if (it != pairsMaps[shard].end())
		return it->second;

	uint32_t offset = pairs[shard].size();

	if (offset >= (1 << (32 - SHARD_BITS)))
		throw std::out_of_range("pair shard overflow");

	pairs[shard].push_back(pair);
	const AttributePair* ptr = &pairs[shard][offset];
	uint32_t rv = (shard << (32 - SHARD_BITS)) + offset;

	pairsMaps[shard][ptr] = rv;
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

void AttributeStore::addAttribute(AttributeSet& attributeSet, std::string const &key, const std::string& v, char minzoom) {
	AttributePair kv(keyStore.key2index(key),v,minzoom);
	bool isHot = AttributePair::isHot(kv, key);
	attributeSet.removePairWithKey(pairStore, kv.keyIndex);
	attributeSet.addPair(pairStore.addPair(kv, isHot));
}
void AttributeStore::addAttribute(AttributeSet& attributeSet, std::string const &key, bool v, char minzoom) {
	AttributePair kv(keyStore.key2index(key),v,minzoom);
	bool isHot = AttributePair::isHot(kv, key);
	attributeSet.removePairWithKey(pairStore, kv.keyIndex);
	attributeSet.addPair(pairStore.addPair(kv, isHot));
}
void AttributeStore::addAttribute(AttributeSet& attributeSet, std::string const &key, float v, char minzoom) {
	AttributePair kv(keyStore.key2index(key),v,minzoom);
	bool isHot = AttributePair::isHot(kv, key);
	attributeSet.removePairWithKey(pairStore, kv.keyIndex);
	attributeSet.addPair(pairStore.addPair(kv, isHot));
}

void AttributeSet::finalizeSet() {
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


AttributeIndex AttributeStore::add(AttributeSet &attributes) {
	// TODO: there's probably a way to use C++ types to distinguish a finalized
	// and non-finalized AttributeSet, which would make this safer.
	attributes.finalizeSet();

	size_t hash = attributes.hash();
	size_t shard = hash % ATTRIBUTE_SHARDS;

	// We can't use the top 2 bits (see OutputObject's bitfields)
	shard = shard >> 2;

	std::lock_guard<std::mutex> lock(setsMutex[shard]);
	lookups++;

	// Do we already have it?
	const auto& existing = setsMaps[shard].find(&attributes);
	if (existing != setsMaps[shard].end()) return existing->second;

	// No, so add and return the index
	uint32_t offset = sets[shard].size();
	if (offset >= (1 << (32 - SHARD_BITS)))
		throw std::out_of_range("set shard overflow");
	sets[shard].push_back(attributes);

	const AttributeSet* ptr = &sets[shard][offset];
	uint32_t rv = (shard << (32 - SHARD_BITS)) + offset;
	setsMaps[shard][ptr] = rv;
	return rv;
}

std::vector<const AttributePair*> AttributeStore::get(AttributeIndex index) const {
	try {
		uint32_t shard = index >> (32 - SHARD_BITS);
		uint32_t offset = index & (~(~0u << (32 - SHARD_BITS)));

		std::lock_guard<std::mutex> lock(setsMutex[shard]);

		const AttributeSet& attrSet = sets[shard].at(offset);

		const size_t n = attrSet.numPairs();

		std::vector<const AttributePair*> rv;
		for (size_t i = 0; i < n; i++) {
			rv.push_back(&pairStore.getPair(attrSet.getPair(i)));
		}

		return rv;
	} catch (std::out_of_range &err) {
		throw std::runtime_error("Failed to fetch attributes at index "+std::to_string(index));
	}
}

void AttributeStore::reportSize() const {
	size_t numAttributeSets = 0;
	for (int i = 0; i < ATTRIBUTE_SHARDS; i++)
		numAttributeSets += sets[i].size();
	std::cout << "Attributes: " << numAttributeSets << " sets from " << lookups.load() << " objects" << std::endl;

	// Print detailed histogram of frequencies of attributes.
	if (false) {
		for (int i = 0; i < ATTRIBUTE_SHARDS; i++) {
			std::cout << "pairsMaps[" << i << "] has " << pairStore.pairsMaps[i].size() << " entries" << std::endl;
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

void AttributeStore::doneReading() {
}
