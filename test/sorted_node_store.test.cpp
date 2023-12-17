#include <iostream>
#include "external/minunit.h"
#include "sorted_node_store.h"

MU_TEST(test_sorted_node_store) {
	SortedNodeStore sns(true);
	mu_check(sns.size() == 0);

	sns.batchStart();

	sns.insert({ {1, {2, 3 } } });

	sns.finalize(1);

	mu_check(sns.size() == 1);

}

MU_TEST_SUITE(test_suite_sorted_node_store) {
	MU_RUN_TEST(test_sorted_node_store);
}

int main() {
	MU_RUN_SUITE(test_suite_sorted_node_store);
	MU_REPORT();
	return MU_EXIT_CODE;
}
