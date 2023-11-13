#include "attribute_store.h"

// AttributeKeyStore
std::deque<std::string> AttributeKeyStore::keys;
std::mutex AttributeKeyStore::keys2index_mutex;
std::unique_ptr<AttributeKeyStoreImmutable> AttributeKeyStore::immutable(
	new AttributeKeyStoreImmutable(
		std::map<const std::string, uint16_t>()
	)
);

// AttributePairStore
thread_local std::random_device dev;
thread_local std::mt19937 rng(dev());
thread_local std::uniform_int_distribution<std::mt19937::result_type> nextShard(0, PAIR_SHARDS-1);

std::vector<std::deque<AttributePair>> AttributePairStore::pairs(PAIR_SHARDS);
std::vector<std::mutex> AttributePairStore::pairsMutex(PAIR_SHARDS);
std::vector<boost::container::flat_map<const AttributePair*, uint32_t, AttributePairStore::key_value_less_ptr>> AttributePairStore::pairsMaps(PAIR_SHARDS);
std::shared_ptr<boost::container::flat_map<const AttributePair*, uint16_t, AttributePairStore::key_value_less_ptr>> AttributePairStore::hotMap(new boost::container::flat_map<const AttributePair*, uint16_t, AttributePairStore::key_value_less_ptr>());

uint32_t AttributePairStore::addPair(const AttributePair& pair) {
	const bool hot = pair.hot();

	if (hot) {
		// This might be a popular pair, worth re-using.
		// Have we already assigned it a hot ID?

		auto captured = hotMap;
		auto entry = captured->find(&pair);

		if (entry != captured->end())
			return entry->second;

		// See if someone else inserted it while we were faffing about.
		std::lock_guard<std::mutex> lock(pairsMutex[0]);
		entry = hotMap->find(&pair);

		if (entry != hotMap->end())
			return entry->second;

		// OK, it's definitely not there. Is there room to add it?
		if (pairs[0].size() < 1 << 16) {

			// We use 0 as a sentinel, so ensure there's at least one entry in the shard.
			if (pairs[0].size() == 0)
				pairs[0].push_back(AttributePair("", vector_tile::Tile_Value(), 0));

			uint16_t newIndex = pairs[0].size();
			boost::container::flat_map<const AttributePair*, uint16_t, AttributePairStore::key_value_less_ptr> newMap(hotMap->begin(), hotMap->end());
			pairs[0].push_back(pair);
			const AttributePair* ptr = &pairs[0][newIndex];
			newMap[ptr] = newIndex;
			hotMap = std::make_shared<boost::container::flat_map<const AttributePair*, uint16_t, AttributePairStore::key_value_less_ptr>>(newMap);
			return newIndex;
		}
	}

	// This is either not a hot key, or there's no room for in the hot shard.
	// Throw it on the pile with the rest of the pairs.
	size_t hash = pair.hash();
	size_t shard = hash % PAIR_SHARDS;
	// Shard 0 is for hot pairs -- pick another shard if it gets selected.
	// TODO: do some other transformation that distributes it better, this is OK
	// for dev.
	if (shard == 0) shard++;

	//size_t shard = nextShard(rng);
	//while (shard == 0) shard = nextShard(rng);
	std::lock_guard<std::mutex> lock(pairsMutex[shard]);

	// TODO: do this without exceptions, good enough for measuring memory
	try {
		uint32_t rv = pairsMaps[shard].at(&pair);
		return rv;
	} catch (std::out_of_range &e) {
		uint32_t offset = pairs[shard].size();
		pairs[shard].push_back(pair);
		const AttributePair* ptr = &pairs[shard][offset];
		uint32_t rv = (shard << (32 - SHARD_BITS)) + offset;

//		std::cout << "addPair(" << rv << ") shard=" << shard << " offset=" << offset << std::endl;
		pairsMaps[shard][ptr] = rv;
		return rv;
	}
};


// AttributeSet

void AttributeSet::add(AttributePair const &kv) {
	uint32_t index = AttributePairStore::addPair(kv);
	values.push_back(index);
}
void AttributeSet::add(std::string const &key, vector_tile::Tile_Value const &v, char minzoom) {
	AttributePair kv(key,v,minzoom);
	add(kv);
}

bool sortFn(const AttributePair* a, const AttributePair* b) {
	if (a->minzoom != b->minzoom)
		return a->minzoom < b->minzoom;

	if (a->keyIndex != b->keyIndex)
		return a->keyIndex < b->keyIndex;

	if (a->value.has_string_value()) return b->value.has_string_value() && a->value.string_value() < b->value.string_value();
	if (a->value.has_bool_value()) return b->value.has_bool_value() && a->value.bool_value() < b->value.bool_value();
	if (a->value.has_float_value()) return b->value.has_float_value() && a->value.float_value() < b->value.float_value();
	throw std::runtime_error("Invalid type in AttributeSet");
}

void AttributeSet::finalize_set() {
	// Memoize the hash value of this set so we don't repeatedly calculate it
	// when inserting/querying sets.
	//
	// TODO: sort its attribute pairs so == tests can do a simple pairwise comparisons
	auto idx = values.size();

	std::vector<const AttributePair*> pairs;
	for(auto const &j: values) {
		// NB: getPair takes a lock
		const auto& i = AttributePairStore::getPair(j);
		pairs.push_back(&i);
	}
	
	// Sort pairs in a stable way, so that we produce the same hash
	std::sort (pairs.begin(), pairs.end(), sortFn);

	for (auto const &i: pairs) {
		boost::hash_combine(idx, i->minzoom);
		boost::hash_combine(idx, i->keyIndex);
		boost::hash_combine(idx, AttributePair::type_index(i->value));

		if(i->value.has_string_value())
			boost::hash_combine(idx, i->value.string_value());
		else if(i->value.has_float_value())
			boost::hash_combine(idx, i->value.float_value());
		else if(i->value.has_bool_value())
			boost::hash_combine(idx, i->value.bool_value());
		else {
			std::cout << "unknown Tile_Value in finalize_set, keyIndex was " << i->keyIndex << std::endl;
		}
	}
	hash_value = idx;
}

// AttributeStore

AttributeIndex AttributeStore::add(AttributeSet &attributes) {
	// TODO: there's probably a way to use C++ types to distinguish a finalized
	// and non-finalized AttributeSet, which would make this safer.
	attributes.finalize_set();

	std::lock_guard<std::mutex> lock(mutex);
	lookups++;

	// Do we already have it?
	auto existing = attribute_sets.find(attributes);
	if (existing != attribute_sets.end()) return existing - attribute_sets.begin();

	// No, so add and return the index
	AttributeIndex idx = static_cast<AttributeIndex>(attribute_sets.size());
	attribute_sets.insert(attributes);
	return idx;
}

// TODO: consider implementing this as an iterator, or returning a cheaper
//       container than a set (vector?)
std::set<AttributePair, AttributePairStore::key_value_less> AttributeStore::get(AttributeIndex index) const {
	try {
		const auto pairIds = attribute_sets.nth(index).key().values;

		std::set<AttributePair, AttributePairStore::key_value_less> rv;
		for (const auto& id: pairIds) {
			rv.insert(AttributePairStore::getPair(id));
		}

		return rv;
	} catch (std::out_of_range &err) {
		throw std::runtime_error("Failed to fetch attributes at index "+std::to_string(index)+" - size is "+std::to_string(attribute_sets.size()));
	}
}

void AttributeStore::reportSize() const {
	std::cout << "Attributes: " << attribute_sets.size() << " sets from " << lookups << " objects" << std::endl;

	// Print detailed histogram of frequencies of attributes.
	if (false) {
		std::map<uint32_t, uint32_t> tagCountDist;

		for (size_t i = 0; i < AttributePairStore::pairs.size(); i++) {
			std::cout << "pairs[" << i << "] has size " << AttributePairStore::pairs[i].size() << std::endl;
			size_t j = 0;
			for (const auto& ap: AttributePairStore::pairs[i]) {
				std::cout << "pairs[" << i << "][" << j << "] keyIndex=" << ap.keyIndex << " minzoom=" << (65+ap.minzoom) << " value=" << ap.value << " key=" << AttributeKeyStore::getKey(ap.keyIndex) << std::endl;
				j++;

			}
		}
		size_t pairs = 0;
		std::map<uint32_t, size_t> uniques;
		for (const auto attr_set: attribute_sets) {
			pairs += attr_set.values.size();

			try {
				tagCountDist[attr_set.values.size()]++;
			} catch (std::out_of_range &err) {
				tagCountDist[attr_set.values.size()] = 1;
			}

			for (const auto attr: attr_set.values) {
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
			std::cout << "attrpair freq= " << entry.second << " key=" << pair.key() <<" value=" << pair.value << std::endl;
		}
	}
}

void AttributeStore::doneReading() {
}
