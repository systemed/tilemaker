#include "attribute_store.h"

#include <iostream>

// AttributeKeyStore
std::deque<std::string> AttributeKeyStore::keys;
std::mutex AttributeKeyStore::keys2indexMutex;
std::map<const std::string, uint16_t> AttributeKeyStore::keys2index;

// AttributePairStore
std::vector<std::deque<AttributePair>> AttributePairStore::pairs(PAIR_SHARDS);
std::vector<std::mutex> AttributePairStore::pairsMutex(PAIR_SHARDS);
std::vector<boost::container::flat_map<const AttributePair*, uint32_t, AttributePairStore::key_value_less_ptr>> AttributePairStore::pairsMaps(PAIR_SHARDS);

uint32_t AttributePairStore::addPair(const AttributePair& pair) {
	const bool hot = pair.hot();

	if (hot) {
		// This might be a popular pair, worth re-using.
		// Have we already assigned it a hot ID?
		std::lock_guard<std::mutex> lock(pairsMutex[0]);
		const auto& it = pairsMaps[0].find(&pair);
		if (it != pairsMaps[0].end())
			return it->second;

		// We use 0 as a sentinel, so ensure there's at least one entry in the shard.
		if (pairs[0].size() == 0)
			pairs[0].push_back(AttributePair("", false, 0));

		uint32_t offset = pairs[0].size();

		if (offset < 1 << 16) {
			pairs[0].push_back(pair);
			const AttributePair* ptr = &pairs[0][offset];
			uint32_t rv = (0 << (32 - SHARD_BITS)) + offset;
			pairsMaps[0][ptr] = rv;
			return rv;
		}
	}

	// This is either not a hot key, or there's no room for in the hot shard.
	// Throw it on the pile with the rest of the pairs.
	size_t hash = pair.hash();

	size_t shard = hash % PAIR_SHARDS;
	// Shard 0 is for hot pairs -- pick another shard if it gets selected.
	if (shard == 0) shard = (hash >> 8) % PAIR_SHARDS;
	if (shard == 0) shard = (hash >> 16) % PAIR_SHARDS;
	if (shard == 0) shard = (hash >> 24) % PAIR_SHARDS;
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

void AttributeSet::add(AttributePair const &kv) {
	uint32_t index = AttributePairStore::addPair(kv);
	add(index);
}

void AttributeSet::add(uint32_t pairIndex) {
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
void AttributeSet::add(std::string const &key, const std::string& v, char minzoom) {
	AttributePair kv(key,v,minzoom);
	add(kv);
}
void AttributeSet::add(std::string const &key, bool v, char minzoom) {
	AttributePair kv(key,v,minzoom);
	add(kv);
}
void AttributeSet::add(std::string const &key, float v, char minzoom) {
	AttributePair kv(key,v,minzoom);
	add(kv);
}

bool sortFn(const AttributePair* a, const AttributePair* b) {
	if (a->minzoom != b->minzoom)
		return a->minzoom < b->minzoom;

	if (a->keyIndex != b->keyIndex)
		return a->keyIndex < b->keyIndex;

	if (a->hasStringValue()) return b->hasStringValue() && a->stringValue() < b->stringValue();
	if (a->hasBoolValue()) return b->hasBoolValue() && a->boolValue() < b->boolValue();
	if (a->hasFloatValue()) return b->hasFloatValue() && a->floatValue() < b->floatValue();
	throw std::runtime_error("Invalid type in AttributeSet");
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
				add(sortMe[i]);
	}
}

// AttributeStore

AttributeIndex AttributeStore::add(AttributeSet &attributes) {
	// TODO: there's probably a way to use C++ types to distinguish a finalized
	// and non-finalized AttributeSet, which would make this safer.
	attributes.finalizeSet();
	std::lock_guard<std::mutex> lock(mutex);
	lookups++;

	// Do we already have it?
	auto existing = attributeSets.find(attributes);
	if (existing != attributeSets.end()) return existing - attributeSets.begin();

	// No, so add and return the index
	AttributeIndex idx = static_cast<AttributeIndex>(attributeSets.size());
	attributeSets.insert(attributes);
	return idx;
}

// TODO: consider implementing this as an iterator, or returning a cheaper
//       container than a set (vector?)
std::set<AttributePair, AttributePairStore::key_value_less> AttributeStore::get(AttributeIndex index) const {
	try {
		const auto& attrSet = attributeSets.nth(index).key();

		const size_t n = attrSet.numPairs();

		std::set<AttributePair, AttributePairStore::key_value_less> rv;
		for (size_t i = 0; i < n; i++)
			rv.insert(AttributePairStore::getPair(attrSet.getPair(i)));

		return rv;
	} catch (std::out_of_range &err) {
		throw std::runtime_error("Failed to fetch attributes at index "+std::to_string(index)+" - size is "+std::to_string(attributeSets.size()));
	}
}

void AttributeStore::reportSize() const {
	std::cout << "Attributes: " << attributeSets.size() << " sets from " << lookups << " objects" << std::endl;

	// Print detailed histogram of frequencies of attributes.
	if (false) {
		for (int i = 0; i < PAIR_SHARDS; i++) {
			std::cout << "pairsMaps[" << i << "] has " << AttributePairStore::pairsMaps[i].size() << " entries" << std::endl;
		}

		std::map<uint32_t, uint32_t> tagCountDist;

		for (size_t i = 0; i < AttributePairStore::pairs.size(); i++) {
			std::cout << "pairs[" << i << "] has size " << AttributePairStore::pairs[i].size() << std::endl;
			size_t j = 0;
			for (const auto& ap: AttributePairStore::pairs[i]) {
				std::cout << "pairs[" << i << "][" << j << "] keyIndex=" << ap.keyIndex << " minzoom=" << (65+ap.minzoom) << " stringValue=" << ap.stringValue() << " floatValue=" << ap.floatValue() << " boolValue=" << ap.boolValue() << " key=" << AttributeKeyStore::getKey(ap.keyIndex) << std::endl;
				j++;

			}
		}
		size_t pairs = 0;
		std::map<uint32_t, size_t> uniques;
		for (const auto& attrSet: attributeSets) {
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
		std::cout << "AttributePairs: " << pairs << ", unique: " << uniques.size() << std::endl;

		for (const auto entry: tagCountDist) {
			std::cout << "tagCountDist " << entry.first << " tags occurs " << entry.second << " times" << std::endl;
		}

		for (const auto entry: uniques) {
			const auto& pair = AttributePairStore::getPair(entry.first);
			// It's useful to occasionally confirm that anything with high freq has hot=1,
			// and also that things with hot=1 have high freq.
			std::cout << "attrpair freq= " << entry.second << " hot=" << (entry.first < 65536 ? 1 : 0) << " key=" << pair.key() <<" stringValue=" << pair.stringValue() << " floatValue=" << pair.floatValue() << " boolValue=" << pair.boolValue() << std::endl;
		}
	}
}

void AttributeStore::doneReading() {
}
