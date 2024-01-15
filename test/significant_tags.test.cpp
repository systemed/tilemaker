#include <iostream>
#include "external/minunit.h"
#include "significant_tags.h"
#include "tag_map.h"

MU_TEST(test_parse_filter) {
	{
		TagFilter expected{true, "foo", ""};
		mu_check(SignificantTags::parseFilter("foo") == expected);
	}

	{
		TagFilter expected{false, "foo", ""};
		mu_check(SignificantTags::parseFilter("~foo") == expected);
	}

	{
		TagFilter expected{true, "foo", "bar"};
		mu_check(SignificantTags::parseFilter("foo=bar") == expected);
	}

	{
		TagFilter expected{false, "foo", "bar"};
		mu_check(SignificantTags::parseFilter("~foo=bar") == expected);
	}

}

MU_TEST(test_invalid_significant_tags) {
	bool threw = false;
	try {
		// Filters must be all accept, or all reject, not a mix.
		SignificantTags tags({"a", "~b"});
	} catch (...) {
		threw = true;
	}

	mu_check(threw);
}

MU_TEST(test_significant_tags) {
	const std::string building = "building";
	const std::string yes = "yes";
	const std::string name = "name";
	const std::string nameValue = "Some name";
	const std::string power = "power";
	const std::string tower = "tower";

	// If created with no list, it's not enabled and all things pass filter.
	// This is the case when people omit `node_keys` or `way_keys`.
	{
		SignificantTags tags;
		TagMap map;
		mu_check(tags.filter(map));
	}

	// If created with empty list, it rejects all things.
	// This is the case when people write `way_keys = {}`, e.g. when creating
	// an extract that only parses nodes.
	{
		std::vector<std::string> empty;
		SignificantTags tags(empty);
		TagMap map;
		mu_check(!tags.filter(map));
	}

	// If created in default-accept mode, it accepts anything with an unmatched tag.
	// This is the case when people write `way_keys = {"-building"}`
	{
		std::vector<std::string> defaultAccept{"~building"};
		SignificantTags tags(defaultAccept);

		{
			TagMap map;
			map.addTag(building, yes);
			mu_check(!tags.filter(map));
		}

		{
			TagMap map;
			map.addTag(building, yes);
			map.addTag(name, nameValue);
			mu_check(tags.filter(map));
		}

	}

	// If created in default-reject mode, it accepts anything with a matched tag.
	// This is the case when people write `way_keys = {"power=tower"}`
	{
		std::vector<std::string> defaultReject{"power=tower"};
		SignificantTags tags(defaultReject);

		{
			TagMap map;
			mu_check(!tags.filter(map));
		}

		{
			TagMap map;
			map.addTag(power, tower);
			mu_check(tags.filter(map));
		}

	}
}

MU_TEST_SUITE(test_suite_significant_tags) {
	MU_RUN_TEST(test_parse_filter);
	MU_RUN_TEST(test_significant_tags);
	MU_RUN_TEST(test_invalid_significant_tags);
}

int main() {
	MU_RUN_SUITE(test_suite_significant_tags);
	MU_REPORT();
	return MU_EXIT_CODE;
}

