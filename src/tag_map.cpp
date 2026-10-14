#include "tag_map.h"
#include <boost/functional/hash.hpp>
#include <iostream>
#include <cstring>

TagMap::TagMap() {
}

void TagMap::reset() {
	// Only clear shards that were used since the last reset.
	for (unsigned i = 0, mask = usedKeyShards; mask; i++, mask >>= 1)
		if (mask & 1)
			keys[i].clear();
	for (unsigned i = 0, mask = usedValueShards; mask; i++, mask >>= 1)
		if (mask & 1)
			values[i].clear();
	usedKeyShards = 0;
	usedValueShards = 0;
}

bool TagMap::empty() const {
	return usedKeyShards == 0;
}

static inline std::size_t hashString(const char* str, size_t size) {
	// This is a pretty crappy hash function in terms of bit
	// avalanching and distribution of output values.
	//
	// But it's very good in terms of speed, which turns out
	// to be the important measure.
	std::size_t hash = size;
	if (hash >= 4) {
		uint32_t word;
		memcpy(&word, str, sizeof(word));
		hash ^= word;
	}

	return hash;
}

uint32_t TagMap::ensureKey(const protozero::data_view& key) {
	const uint16_t shard = hashString(key.data(), key.size()) & (SHARDS - 1);
	std::vector<KeySlot>& bucket = keys[shard];
	const uint32_t size = key.size();
	for (size_t i = 0; i < bucket.size(); i++)
		if (bucket[i].size == size && memcmp(bucket[i].data, key.data(), size) == 0)
			return shard << 16 | i;

	bucket.push_back(KeySlot{key.data(), size, 0});
	usedKeyShards |= 1u << shard;
	return shard << 16 | (bucket.size() - 1);
}

uint32_t TagMap::ensureValue(const protozero::data_view& value) {
	const uint16_t shard = hashString(value.data(), value.size()) & (SHARDS - 1);
	std::vector<protozero::data_view>& bucket = values[shard];
	for (size_t i = 0; i < bucket.size(); i++)
		if (bucket[i] == value)
			return shard << 16 | i;

	bucket.push_back(value);
	usedValueShards |= 1u << shard;
	return shard << 16 | (bucket.size() - 1);
}

void TagMap::addTag(const protozero::data_view& key, const protozero::data_view& value) {
	uint32_t valueLoc = ensureValue(value);
	uint32_t keyLoc = ensureKey(key);

	keys[keyLoc >> 16][keyLoc & 0xFFFF].valueLoc = valueLoc;
}

int64_t TagMap::getKey(const char* key, size_t size) const {
	// Return -1 if key not found, else return its keyLoc.
	const uint16_t shard = hashString(key, size) & (SHARDS - 1);
	const std::vector<KeySlot>& bucket = keys[shard];
	for (size_t i = 0; i < bucket.size(); i++) {
		if (bucket[i].size != size)
			continue;

		if (memcmp(bucket[i].data, key, size) == 0)
			return shard << 16 | i;
	}

	return -1;
}

int64_t TagMap::getValue(const char* value, size_t size) const {
	// Return -1 if value not found, else return its valueLoc.
	const uint16_t shard = hashString(value, size) & (SHARDS - 1);
	const std::vector<protozero::data_view>& bucket = values[shard];
	for (size_t i = 0; i < bucket.size(); i++) {
		if (bucket[i].size() != size)
			continue;

		if (memcmp(bucket[i].data(), value, size) == 0)
			return shard << 16 | i;
	}

	return -1;
}

const protozero::data_view* TagMap::getValueFromKey(uint32_t keyLoc) const {
	const uint32_t valueLoc = keys[keyLoc >> 16][keyLoc & 0xFFFF].valueLoc;
	return &values[valueLoc >> 16][valueLoc & 0xFFFF];
}

const protozero::data_view* TagMap::getValue(uint32_t valueLoc) const {
	return &values[valueLoc >> 16][valueLoc & 0xFFFF];
}

boost::container::flat_map<std::string, std::string> TagMap::exportToBoostMap() const {
	boost::container::flat_map<std::string, std::string> rv;

	for (size_t i = 0; i < keys.size(); i++) {
		for (size_t j = 0; j < keys[i].size(); j++) {
			const KeySlot& key = keys[i][j];
			const protozero::data_view& value = *getValue(key.valueLoc);
			rv[std::string(key.data, key.size)] = std::string(value.data(), value.size());
		}
	}

	return rv;
}

TagMap::Iterator TagMap::begin() const {
	size_t shard = 0;
	while(keys.size() > shard && keys[shard].size() == 0)
		shard++;

	return Iterator{*this, shard, 0};
}

TagMap::Iterator TagMap::end() const {
	return Iterator{*this, keys.size(), 0};
}

bool TagMap::Iterator::operator!=(const Iterator& other) const {
	return other.shard != shard || other.offset != offset;
}

void TagMap::Iterator::operator++() {
	++offset;
	if (offset >= map.keys[shard].size()) {
		offset = 0;
		shard++;
		// Advance to the next non-empty shard.
		while(map.keys.size() > shard && map.keys[shard].size() == 0)
			shard++;
	}
}

Tag TagMap::Iterator::operator*() const {
	const KeySlot& key = map.keys[shard][offset];
	return Tag{
		protozero::data_view(key.data, key.size),
		*map.getValue(key.valueLoc)
	};
}
