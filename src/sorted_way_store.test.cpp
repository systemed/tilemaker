#include "minunit.h"
#include "sorted_way_store.h"

MU_TEST(test_way_store) {
	SortedWayStore sws;

	std::vector<std::pair<WayID, std::vector<NodeID>>> ways;
	std::vector<NodeID> shortWay;
	shortWay.push_back(123);
	ways.push_back(std::make_pair(1, shortWay));

	std::vector<NodeID> longWay;
	for(int i = 200; i < 300; i++)
		longWay.push_back(i);
	ways.push_back(std::make_pair(65536, longWay));
	ways.push_back(std::make_pair(131072, longWay));

	sws.insertNodes(ways);
	sws.finalize(1);

	const auto& rv = sws.at(1);
	mu_check(rv.size() == 1);
	// TODO: requires a node store
	// mu_check(rv[0] == 123);
	mu_check(sws.size() == 3);
}

MU_TEST_SUITE(test_suite) {
	MU_RUN_TEST(test_way_store);
}

int main() {
	MU_RUN_SUITE(test_suite);
	MU_REPORT();
	return MU_EXIT_CODE;
}
