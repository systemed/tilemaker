#ifndef _TAG_MAP_H
#define _TAG_MAP_H

#include <array>
#include <vector>
#include <string>
#include <boost/container/flat_map.hpp>
#include <protozero/data_view.hpp>

// We track tags in a special structure, which enables some tricks when
// doing Lua interop.
//
// The alternative is a std::map - but often, our map is quite small.
// It's preferable to have a small set of vectors and do linear search.
//
// Further, we can avoid passing std::string from Lua -> C++ in some cases
// by first checking to see if the string we would have passed is already
// stored in our tag map, and passing a reference to its location.

// Assumptions:
// 1. Not thread-safe
//      This is OK because we have 1 instance of OsmLuaProcessing per thread.
// 2. Lifetime of map is less than lifetime of keys/values that are passed
//      This is true since the strings are owned by the protobuf block reader
// 3. Max number of tag values will fit in a short
//      OSM limit is 5,000 tags per object
//
// The hashed form is built lazily, on the first lookup: most ways are answered by the
// way_function memo from the tags as added (see added()) and never need it.
struct Tag {
	protozero::data_view key;
	protozero::data_view value;
};

class TagMap {
public:
	TagMap();
	void reset();

	bool empty() const { return tags.empty(); }
	void addTag(const protozero::data_view& key, const protozero::data_view& value) {
		tags.push_back(Tag{key, value});
		if (built) insert(key, value);
	}

	// The tags in the order they were added (a key added twice: the last value counts)
	const std::vector<Tag>& added() const { return tags; }

	// Return -1 if key not found, else return its keyLoc.
	int64_t getKey(const char* key, size_t size) const;

	// Return -1 if value not found, else return its keyLoc.
	int64_t getValue(const char* key, size_t size) const;

	const protozero::data_view* getValueFromKey(uint32_t keyLoc) const;
	const protozero::data_view* getValue(uint32_t valueLoc) const;

	boost::container::flat_map<std::string, std::string> exportToBoostMap() const;

	struct Iterator {
		const TagMap& map;
		size_t shard = 0;
		size_t offset = 0;

		bool operator!=(const Iterator& other) const;
		void operator++();
		Tag operator*() const;
	};

	Iterator begin() const;
	Iterator end() const;

private:
	static constexpr size_t SHARDS = 16;
	static_assert((SHARDS & (SHARDS - 1)) == 0, "SHARDS must be a power of two");

	// Keys are stored flat (pointer + size inline) with their valueLoc, so a
	// lookup miss can reject a candidate by size without dereferencing it.
	struct KeySlot {
		const char* data;
		uint32_t size;
		uint32_t valueLoc;
		uint64_t prefix;	// the first (up to) 8 bytes, see keyPrefix
	};

	uint32_t ensureKey(const protozero::data_view& key) const;
	uint32_t ensureValue(const protozero::data_view& value) const;
	void insert(const protozero::data_view& key, const protozero::data_view& value) const;
	void ensureBuilt() const { if (!built) build(); }
	void build() const;

	std::vector<Tag> tags;

	// The hashed form of `tags`, built on demand
	mutable bool built = false;
	mutable std::array<std::vector<KeySlot>, SHARDS> keys;
	mutable std::array<std::vector<protozero::data_view>, SHARDS> values;

	// Bit i set => keys[i] (resp. values[i]) may be non-empty.
	mutable uint16_t usedKeyShards = 0;
	mutable uint16_t usedValueShards = 0;
};

#endif //_TAG_MAP_H
