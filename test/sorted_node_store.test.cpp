#include <iostream>
#include "external/minunit.h"
#include "sorted_node_store.h"

MU_TEST(test_sorted_node_store) {
	SortedNodeStore s1(true), s2(true);
	mu_check(s1.size() == 0);
	mu_check(s2.size() == 0);

	s1.batchStart();
	s2.batchStart();

	s1.insert({ {1, {2, 3 } } });
	s2.insert({ {2, {3, 4 } } });

	s1.finalize(1);
	s2.finalize(1);

	mu_check(s1.size() == 1);
	mu_check(s1.at(1) == LatpLon({2, 3}));
	mu_check(s2.size() == 1);
	mu_check(s2.at(2) == LatpLon({3, 4}));
}

MU_TEST_SUITE(test_suite_sorted_node_store) {
	MU_RUN_TEST(test_sorted_node_store);
}

int main() {
	MU_RUN_SUITE(test_suite_sorted_node_store);
	MU_REPORT();
	return MU_EXIT_CODE;
}
