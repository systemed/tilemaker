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
	// TODO: do some other transformation that distributes it better, this is OK
	// for dev.
	if (shard == 0) shard++;

	std::lock_guard<std::mutex> lock(pairsMutex[shard]);
	const auto& it = pairsMaps[shard].find(&pair);
	if (it != pairsMaps[shard].end())
		return it->second;

	uint32_t offset = pairs[shard].size();
	pairs[shard].push_back(pair);
	const AttributePair* ptr = &pairs[shard][offset];
	uint32_t rv = (shard << (32 - SHARD_BITS)) + offset;

	pairsMaps[shard][ptr] = rv;
	return rv;
};


// AttributeSet

void AttributeSet::add(AttributePair const &kv) {
	uint32_t index = AttributePairStore::addPair(kv);
	values.push_back(index);
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

	if (a->has_string_value()) return b->has_string_value() && a->string_value() < b->string_value();
	if (a->has_bool_value()) return b->has_bool_value() && a->bool_value() < b->bool_value();
	if (a->has_float_value()) return b->has_float_value() && a->float_value() < b->float_value();
	throw std::runtime_error("Invalid type in AttributeSet");
}

void AttributeSet::finalize_set() {
	// Ensure that values are sorted, so we have a canonical representation of
	// an attribute set.
	sort(values.begin(), values.end());
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
				std::cout << "pairs[" << i << "][" << j << "] keyIndex=" << ap.keyIndex << " minzoom=" << (65+ap.minzoom) << " string_value=" << ap.string_value() << " float_value=" << ap.float_value() << " bool_value=" << ap.bool_value() << " key=" << AttributeKeyStore::getKey(ap.keyIndex) << std::endl;
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
			// It's useful to occasionally confirm that anything with high freq has hot=1,
			// and also that things with hot=1 have high freq.
			std::cout << "attrpair freq= " << entry.second << " hot=" << (entry.first < 65536 ? 1 : 0) << " key=" << pair.key() <<" string_value=" << pair.string_value() << " float_value=" << pair.float_value() << " bool_value=" << pair.bool_value() << std::endl;
		}
	}
}

void AttributeStore::doneReading() {
}
