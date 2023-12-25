#include <iostream>
#include <algorithm>
#include "external/minunit.h"
#include "deque_map.h"

MU_TEST(test_deque_map) {
	DequeMap<std::string> strs;

	mu_check(strs.size() == 0);
	mu_check(!strs.full());
	mu_check(strs.find("foo") == -1);
	mu_check(strs.add("foo") == 0);
	mu_check(!strs.full());
	mu_check(strs.find("foo") == 0);
	mu_check(strs.size() == 1);
	mu_check(strs.add("foo") == 0);
	mu_check(strs.size() == 1);
	mu_check(strs.add("bar") == 1);
	mu_check(strs.size() == 2);
	mu_check(strs.add("aardvark") == 2);
	mu_check(strs.size() == 3);
	mu_check(strs.add("foo") == 0);
	mu_check(strs.add("bar") == 1);
	mu_check(strs.add("quux") == 3);
	mu_check(strs.size() == 4);

	mu_check(strs.at(0) == "foo");
	mu_check(strs[0] == "foo");
	mu_check(strs.at(1) == "bar");
	mu_check(strs[1] == "bar");
	mu_check(strs.at(2) == "aardvark");
	mu_check(strs[2] == "aardvark");
	mu_check(strs.at(3) == "quux");
	mu_check(strs[3] == "quux");

	std::vector<std::string> rv;
	for (std::string x : strs) {
		rv.push_back(x);
	}
	mu_check(rv[0] == "aardvark");
	mu_check(rv[1] == "bar");
	mu_check(rv[2] == "foo");
	mu_check(rv[3] == "quux");

	DequeMap<std::string> boundedMap(1);
	mu_check(!boundedMap.full());
	mu_check(boundedMap.add("foo") == 0);
	mu_check(boundedMap.add("foo") == 0);
	mu_check(boundedMap.full());
	mu_check(boundedMap.add("bar") == -1);
	boundedMap.clear();
	mu_check(!boundedMap.full());
	mu_check(boundedMap.find("foo") == -1);
	mu_check(boundedMap.add("bar") == 0);
	mu_check(boundedMap.add("bar") == 0);
	mu_check(boundedMap.full());
}

MU_TEST_SUITE(test_suite_deque_map) {
	MU_RUN_TEST(test_deque_map);
}

int main() {
	MU_RUN_SUITE(test_suite_deque_map);
	MU_REPORT();
	return MU_EXIT_CODE;
}
