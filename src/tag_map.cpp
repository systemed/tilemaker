#include "tag_map.h"
#include <boost/functional/hash.hpp>
#include <iostream>

TagMap::TagMap() {
	keys.resize(16);
	key2value.resize(16);
	values.resize(16);
}

void TagMap::reset() {
	for (int i = 0; i < 16; i++) {
		keys[i].clear();
		key2value[i].clear();
		values[i].clear();
	}
}

const std::size_t hashString(const std::string& str) {
	// This is a pretty crappy hash function in terms of bit
	// avalanching and distribution of output values.
	//
	// But it's very good in terms of speed, which turns out
	// to be the important measure.
	std::size_t hash = str.size();
	if (hash >= 4)
		hash ^= *(uint32_t*)str.data();

	return hash;
}

const std::size_t hashString(const char* str, size_t size) {
	// This is a pretty crappy hash function in terms of bit
	// avalanching and distribution of output values.
	//
	// But it's very good in terms of speed, which turns out
	// to be the important measure.
	std::size_t hash = size;
	if (hash >= 4)
		hash ^= *(uint32_t*)str;

	return hash;
}

uint32_t TagMap::ensureString(
	std::vector<std::vector<const std::string*>>& vector,
	const std::string& value
) {
	std::size_t hash = hashString(value);

	const uint16_t shard = hash % vector.size();
	for (int i = 0; i < vector[shard].size(); i++)
		if (*(vector[shard][i]) == value)
			return shard << 16 | i;

	vector[shard].push_back(&value);
	return shard << 16 | (vector[shard].size() - 1);
}


void TagMap::addTag(const std::string& key, const std::string& value) {
	uint32_t valueLoc = ensureString(values, value);
//	std::cout << "valueLoc = " << valueLoc << std::endl;
	uint32_t keyLoc = ensureString(keys, key);
//	std::cout << "keyLoc = " << keyLoc << std::endl;


	const uint16_t shard = keyLoc >> 16;
	const uint16_t pos = keyLoc;
//	std::cout << "shard=" << shard << ", pos=" << pos << std::endl;
	if (key2value[shard].size() <= pos) {
//		std::cout << "growing shard" << std::endl;
		key2value[shard].resize(pos + 1);
	}

	key2value[shard][pos] = valueLoc;
}

const std::string* TagMap::getTag(const std::string& key) const {
	// Returns nullptr if absent, else pointer to value.
	std::size_t hash = hashString(key);

	const uint16_t shard = hash % keys.size();
	for (int i = 0; i < keys[shard].size(); i++)
		if (*(keys[shard][i]) == key) {
			const uint32_t valueLoc = key2value[shard][i];
			return values[valueLoc >> 16][valueLoc & 0xFFFF];
		}

	return nullptr;
}

int64_t TagMap::getKey(const char* key, size_t size) const {
	// Return -1 if key not found, else return its keyLoc.
	std::size_t hash = hashString(key, size);

	const uint16_t shard = hash % keys.size();
	for (int i = 0; i < keys[shard].size(); i++) {
		const std::string& candidate = *keys[shard][i];
	  if (candidate.size() != size)
			continue;

		if (memcmp(candidate.data(), key, size) == 0)
			return shard << 16 | i;
	}

	return -1;
}

int64_t TagMap::getValue(const char* value, size_t size) const {
	// Return -1 if value not found, else return its valueLoc.
	std::size_t hash = hashString(value, size);

	const uint16_t shard = hash % values.size();
	for (int i = 0; i < values[shard].size(); i++) {
		const std::string& candidate = *values[shard][i];
	  if (candidate.size() != size)
			continue;

		if (memcmp(candidate.data(), value, size) == 0)
			return shard << 16 | i;
	}

	return -1;
}

const std::string* TagMap::getValueFromKey(uint32_t keyLoc) const {
	const uint32_t valueLoc = key2value[keyLoc >> 16][keyLoc & 0xFFFF];
	return values[valueLoc >> 16][valueLoc & 0xFFFF];
}

const std::string* TagMap::getValue(uint32_t valueLoc) const {
	return values[valueLoc >> 16][valueLoc & 0xFFFF];
}

boost::container::flat_map<std::string, std::string> TagMap::exportToBoostMap() const {
	boost::container::flat_map<std::string, std::string> rv;

	for (int i = 0; i < keys.size(); i++) {
		for (int j = 0; j < keys[i].size(); j++) {
			uint32_t valueLoc = key2value[i][j];
			rv[*keys[i][j]] = *values[valueLoc >> 16][valueLoc & 0xFFFF];
		}
	}

	return rv;
}
