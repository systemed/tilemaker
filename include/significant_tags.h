#ifndef SIGNIFICANT_TAGS_H
#define SIGNIFICANT_TAGS_H

#include <string>
#include <vector>

class TagMap;
// Data structures to permit users to express filters on which nodes/ways
// to be accepted.
//
// Filters are of the shape: [~]key-name[=value-name]
//
// When a tilde is present, the filter's meaning is inverted.

struct TagFilter {
	bool accept;
	std::string key;
	std::string value;

	bool operator==(const TagFilter& other) const {
		return accept == other.accept && key == other.key && value == other.value;
	}
};

class SignificantTags {
public:
	SignificantTags();
	SignificantTags(std::vector<std::string> rawTags);
	bool filter(const TagMap& tags) const;

	static TagFilter parseFilter(std::string rawTag);
	bool enabled() const;

private:
	bool enabled_;
	std::vector<TagFilter> filters;
};

#endif
