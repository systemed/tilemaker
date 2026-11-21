#include <iostream>
#include <algorithm>
#include "external/minunit.h"
#include "cluster.h"

typedef std::vector<std::pair<int,int>> Points;

MU_TEST(test_cluster_trivial) {
	mu_check(clusterPoints({}, 100).empty());
	mu_check(clusterPoints({{5,5}}, 100) == std::vector<size_t>({0}));

	// No distance: nothing clusters
	Points points = {{0,0}, {1,1}, {2,2}};
	mu_check(clusterPoints(points, 0).size() == 3);
}

MU_TEST(test_cluster_keeps_outliers) {
	// Ten points in a town, and one lone point far outside it
	Points points;
	for (int i = 0; i < 10; i++) points.push_back({1000 + (i%4)*10, 1000 + (i/4)*10});
	points.push_back({3000, 3000});

	std::vector<size_t> result = clusterPoints(points, 100);
	mu_check(result.size() == 2);
	mu_check(std::find(result.begin(), result.end(), 10) != result.end());
	mu_check(std::is_sorted(result.begin(), result.end()));

	// The town's representative is one of the town's points, near its middle
	const std::pair<int,int> &town = points[result[0]];
	mu_check(town.first >= 1000 && town.first <= 1030);
	mu_check(town.second >= 1000 && town.second <= 1020);
	mu_check(town.first == 1010 || town.first == 1020);
	mu_check(town.second == 1010);
}

MU_TEST(test_cluster_separate_groups) {
	// Two groups further apart than the distance stay separate
	Points points = {{0,0}, {10,0}, {0,10}, {500,0}, {510,0}, {500,10}};
	std::vector<size_t> result = clusterPoints(points, 100);
	mu_check(result.size() == 2);
	mu_check(points[result[0]].first < 100);
	mu_check(points[result[1]].first >= 500);
}

MU_TEST(test_cluster_order_independent) {
	Points points = {{0,0}, {50,0}, {100,0}, {150,0}, {200,0}, {250,0}, {300,0}, {-200,-200}};
	Points reversed(points.rbegin(), points.rend());

	std::vector<size_t> a = clusterPoints(points, 120);
	std::vector<size_t> b = clusterPoints(reversed, 120);
	mu_check(a.size() == b.size());

	Points pa, pb;
	for (size_t i : a) pa.push_back(points[i]);
	for (size_t i : b) pb.push_back(reversed[i]);
	std::sort(pa.begin(), pa.end());
	std::sort(pb.begin(), pb.end());
	mu_check(pa == pb);
}

MU_TEST(test_cluster_negative_coordinates) {
	// Points in the buffer beyond the tile edge have negative coordinates
	Points points = {{-10,-10}, {5,5}, {-200,-200}};
	std::vector<size_t> result = clusterPoints(points, 50);
	mu_check(result.size() == 2);
}

MU_TEST(test_cluster_attribute_key) {
	AttributeStore store;
	AttributeSet named, unnamed, other;
	store.addAttribute(named, "class", protozero::data_view("pub", 3), 0);
	store.addAttribute(named, "name", protozero::data_view("The Crown", 9), 14);
	store.addAttribute(unnamed, "class", protozero::data_view("pub", 3), 0);
	store.addAttribute(other, "class", protozero::data_view("cafe", 4), 0);
	AttributeIndex namedIndex = store.add(named);
	AttributeIndex unnamedIndex = store.add(unnamed);
	AttributeIndex otherIndex = store.add(other);

	std::string a, b, c;
	clusterAttributeKey(store, namedIndex, 12, a);
	clusterAttributeKey(store, unnamedIndex, 12, b);
	clusterAttributeKey(store, otherIndex, 12, c);
	mu_check(a == b);
	mu_check(a != c);

	clusterAttributeKey(store, namedIndex, 14, a);
	clusterAttributeKey(store, unnamedIndex, 14, b);
	mu_check(a != b);
}

MU_TEST_SUITE(test_suite_cluster) {
	MU_RUN_TEST(test_cluster_trivial);
	MU_RUN_TEST(test_cluster_keeps_outliers);
	MU_RUN_TEST(test_cluster_separate_groups);
	MU_RUN_TEST(test_cluster_order_independent);
	MU_RUN_TEST(test_cluster_negative_coordinates);
	MU_RUN_TEST(test_cluster_attribute_key);
}

int main() {
	MU_RUN_SUITE(test_suite_cluster);
	MU_REPORT();
	return MU_EXIT_CODE;
}
