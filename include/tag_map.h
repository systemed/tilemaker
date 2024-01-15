#ifndef _TAG_MAP_H
#define _TAG_MAP_H

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
struct Tag {
	protozero::data_view key;
	protozero::data_view value;
};

class TagMap {
public:
	TagMap();
	void reset();

	bool empty() const;
	void addTag(const protozero::data_view& key, const protozero::data_view& value);

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
	uint32_t ensureString(
		std::vector<std::vector<const protozero::data_view*>>& vector,
		const protozero::data_view& value
	);


	std::vector<std::vector<const protozero::data_view*>> keys;
	std::vector<std::vector<uint32_t>> key2value;
	std::vector<std::vector<const protozero::data_view*>> values;
};

#endif _TAG_MAP_H
