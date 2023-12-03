#ifndef _TAG_MAP_H
#define _TAG_MAP_H

#include <vector>
#include <string>
#include <boost/container/flat_map.hpp>

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
// 1. Not thread-safe.
// 2. Lifetime of map is less than lifetime of keys/values that are passed.
class TagMap {
public:
	TagMap();
	void reset();

	void addTag(const std::string& key, const std::string& value);
	const std::string* getTag(const std::string& key) const;

	boost::container::flat_map<std::string, std::string> exportToBoostMap() const;

private:
	uint32_t ensureString(
		std::vector<std::vector<const std::string*>>& vector,
		const std::string& value
	);


	std::vector<std::vector<const std::string*>> keys;
	std::vector<std::vector<uint32_t>> key2value;
	std::vector<std::vector<const std::string*>> values;
};

#endif _TAG_MAP_H
