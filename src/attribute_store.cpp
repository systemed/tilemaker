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
// TODO: can we use a hash instead of an RNG for assigning? For now, we have hot spots
// until we handle "hot" attribute pairs (eg tunnel=0).
thread_local std::random_device dev;
thread_local std::mt19937 rng(dev());
thread_local std::uniform_int_distribution<std::mt19937::result_type> nextShard(0, PAIR_SHARDS-1);

std::vector<std::deque<AttributePair>> AttributePairStore::pairRefs(PAIR_SHARDS);
std::vector<std::mutex> AttributePairStore::pairRefs_mutex(PAIR_SHARDS);

uint32_t AttributePairStore::addPair(const AttributePair& pair) {
	// TODO: is it worth using a hash? We could also randomly assign pairs
	// to shards.
	// The pair has a `keyIndex`, so subsequent runs of the program will
	// likely have a different hash value for the pair anyway, so there's
	// no reproduceability benefit.
	//
	// Oh, yeah, until we do hot/cold, random assignment is probably needed...
//		uint32_t hashValue = pair.hash();
//		size_t shard = hashValue >> (32 - SHARD_BITS);
//	timespec start, end;
//	clock_gettime(CLOCK_MONOTONIC, &start);

	size_t shard = nextShard(rng);
//	uint64_t shardy = 1e9 * (end.tv_sec - start.tv_sec) + end.tv_nsec - start.tv_nsec;
//	size_t shard = shardy % PAIR_SHARDS;
//	std::cout << "nextShard=" << shard << std::endl << std::flush;
	std::lock_guard<std::mutex> lock(pairRefs_mutex[shard]);
//		std::cout << "pairRefs_mutex[" << shard << "]=" << pairRefs_mutex[shard].native_handle() << std::endl;
	uint32_t offset = pairRefs[shard].size();
	pairRefs[shard].push_back(pair);
	uint32_t rv = (shard << (32 - SHARD_BITS)) + offset;
//		std::cout << "adding pair to shard=" << shard << ", offset=" << offset << ", index=" << rv;
	return rv;
};


// AttributeSet

void AttributeSet::add(AttributePair const &kv) {
	// TODO: implement hot/cold strategy to re-use popular pairs
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
		else
			boost::hash_combine(idx, i->value.bool_value());
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
std::set<AttributePair, AttributeSet::key_value_less> AttributeStore::get(AttributeIndex index) const {
	try {
		const auto pairIds = attribute_sets.nth(index).key().values;

		std::set<AttributePair, AttributeSet::key_value_less> rv;
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

		size_t pairs = 0;
		std::map<AttributePair, size_t, AttributeSet::key_value_less> uniques;
		for (const auto attr_set: attribute_sets) {
			pairs += attr_set.values.size();

			try {
				tagCountDist[attr_set.values.size()]++;
			} catch (std::out_of_range &err) {
				tagCountDist[attr_set.values.size()] = 1;
			}

			for (const auto attr: attr_set.values) {
				try {
					uniques[AttributePairStore::getPair(attr)]++;
				} catch (std::out_of_range &err) {
					uniques[AttributePairStore::getPair(attr)] = 1;
				}
			}
		}
		std::cout << "AttributePairs: " << pairs << ", unique: " << uniques.size() << std::endl;

		for (const auto entry: tagCountDist) {
			std::cout << "tagCountDist " << entry.first << " tags occurs " << entry.second << " times" << std::endl;
		}

		for (const auto entry: uniques) {
			std::cout << "attrpair freq= " << entry.second << " key=" << entry.first.key() <<" value=" << entry.first.value << std::endl;
		}
	}
}

void AttributeStore::doneReading() {
}
