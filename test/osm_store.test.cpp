#include <iostream>
#include "external/minunit.h"
#include "osm_store.h"

MU_TEST(test_usedways_grows_for_first_index) {
	UsedWays usedWays;
	usedWays.insert(0);
	mu_check(usedWays.at(0));
}

MU_TEST(test_usedways_grows_at_current_size) {
	UsedWays usedWays;
	usedWays.insert(0);
	usedWays.insert(256);
	mu_check(usedWays.at(256));
}

MU_TEST_SUITE(test_suite_osm_store) {
	MU_RUN_TEST(test_usedways_grows_for_first_index);
	MU_RUN_TEST(test_usedways_grows_at_current_size);
}

int main() {
	MU_RUN_SUITE(test_suite_osm_store);
	MU_REPORT();
	return MU_EXIT_CODE;
}
