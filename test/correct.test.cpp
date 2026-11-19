#include <iostream>
#include <cmath>
#include <string>
#include "external/minunit.h"
#include "geometry/correct.hpp"

typedef boost::geometry::model::d2::point_xy<double> point_t;
typedef boost::geometry::model::polygon<point_t> polygon_t;
typedef boost::geometry::model::multi_polygon<polygon_t> multi_polygon_t;

static bool near(double a, double b) { return std::abs(a - b) < 1e-9; }

MU_TEST(test_correct_merges_overlapping_holes) {
	// Two overlapping holes are merged into one; a disjoint hole is kept as is
	polygon_t input;
	boost::geometry::read_wkt(
		"POLYGON((0 0,0 10,10 10,10 0,0 0),"
		"(1 1,3 1,3 3,1 3,1 1),(2 2,4 2,4 4,2 4,2 2),(6 6,7 6,7 7,6 7,6 6))", input);
	multi_polygon_t output;
	geometry::correct(input, output);

	mu_check(output.size() == 1);
	mu_check(output[0].inners().size() == 2);
	mu_check(near(boost::geometry::area(output), 100 - 7 - 1));
	mu_check(boost::geometry::is_valid(output));
}

MU_TEST(test_correct_merges_overlapping_outers) {
	multi_polygon_t input;
	boost::geometry::read_wkt(
		"MULTIPOLYGON(((0 0,0 2,2 2,2 0,0 0)),((1 1,1 3,3 3,3 1,1 1)),((5 5,5 6,6 6,6 5,5 5)))", input);
	multi_polygon_t output;
	geometry::correct(input, output);

	mu_check(output.size() == 2);
	mu_check(near(boost::geometry::area(output), 4 + 4 - 1 + 1));
	mu_check(boost::geometry::is_valid(output));
}

MU_TEST(test_correct_many_disjoint_holes) {
	// 400 disjoint holes, e.g. a lake with many islands: all are kept
	std::string wkt = "POLYGON((0 0,0 100,100 100,100 0,0 0)";
	for (int x = 0; x < 20; x++)
		for (int y = 0; y < 20; y++) {
			double x0 = 1 + x * 5, y0 = 1 + y * 5;
			wkt += ",(" + std::to_string(x0) + " " + std::to_string(y0) + "," + std::to_string(x0 + 2) + " " + std::to_string(y0) + "," +
				std::to_string(x0 + 2) + " " + std::to_string(y0 + 2) + "," + std::to_string(x0) + " " + std::to_string(y0 + 2) + "," +
				std::to_string(x0) + " " + std::to_string(y0) + ")";
		}
	wkt += ")";
	polygon_t input;
	boost::geometry::read_wkt(wkt, input);
	multi_polygon_t output;
	geometry::correct(input, output);

	mu_check(output.size() == 1);
	mu_check(output[0].inners().size() == 400);
	mu_check(near(boost::geometry::area(output), 100 * 100 - 400 * 4));
	mu_check(boost::geometry::is_valid(output));
}

MU_TEST_SUITE(test_suite_correct) {
	MU_RUN_TEST(test_correct_merges_overlapping_holes);
	MU_RUN_TEST(test_correct_merges_overlapping_outers);
	MU_RUN_TEST(test_correct_many_disjoint_holes);
}

int main() {
	MU_RUN_SUITE(test_suite_correct);
	MU_REPORT();
	return MU_EXIT_CODE;
}
