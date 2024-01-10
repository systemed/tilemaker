#include <stdexcept>
#include "significant_tags.h"
#include "tag_map.h"

TagFilter SignificantTags::parseFilter(std::string rawTag) {
	TagFilter rv { true };

	std::string input = rawTag;

	if (input.size() > 0 && input[0] == '~') {
		rv.accept = false;
		input = input.substr(1);
	}

	size_t n = input.find("=");

	if (n == std::string::npos) {
		rv.key = input;
		return rv;
	}

	rv.key = input.substr(0, n);
	rv.value = input.substr(n + 1);

	return rv;
}

SignificantTags::SignificantTags(): enabled_(false) {}

SignificantTags::SignificantTags(std::vector<std::string> rawTags): enabled_(true) {
	for (const std::string& rawTag : rawTags) {
		filters.push_back(parseFilter(rawTag));
	}

	if (filters.empty())
		return;

	bool accept = filters[0].accept;

	size_t i = 0;
	for (const auto& filter : filters) {
		if (filter.accept != accept) {
			throw std::runtime_error("cannot mix reject and accept filters: " + rawTags[0] + ", " + rawTags[i]);
		}
		i++;
	}
}

bool SignificantTags::enabled() const { return enabled_; }

bool SignificantTags::filter(const TagMap& tags) const {
	if (!enabled_)
		return true;

	if (filters.empty())
		return false;

	bool defaultReject = filters[0].accept;

	if (defaultReject) {
		// There must be at least one tag matched by the filters.
		for (const Tag& tag : tags) {
			for (const TagFilter& filter : filters) {
				if (filter.key == tag.key && (filter.value.empty() || filter.value == tag.value))
					return true;
			}
		}

		return false;
	}

	// There must be at least one tag not matched by any filters.
	for (const Tag& tag : tags) {
		// If no filters match this tag,
		bool hadMatch = false;
		for (const TagFilter& filter : filters) {
			if (filter.key == tag.key && (filter.value.empty() || filter.value == tag.value)) {
				hadMatch = true;
				break;
			}
		}

		if (!hadMatch)
			return true;
	}

	return false;
}
