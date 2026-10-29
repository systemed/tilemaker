#include <iostream>
#include <cmath>
#include "external/minunit.h"
#include "geom.h"

static MultiPolygon fromWkt(const char *wkt) {
	MultiPolygon mp;
	geom::read_wkt(wkt, mp);
	geom::correct(mp);
	return mp;
}

// rect_clip must give a valid result of the expected area
static void checkClip(const char *wkt, const Box &box, std::size_t expectedPolygons, double expectedArea) {
	MultiPolygon mp = fromWkt(wkt);
	mu_check(geom::is_valid(mp));

	MultiPolygon clipped;
	mu_check(rect_clip(mp, box, clipped));
	mu_check(geom::is_valid(clipped));
	mu_check(clipped.size() == expectedPolygons);
	mu_check(std::abs(geom::area(clipped) - expectedArea) < 1e-9);
}

MU_TEST(test_rect_clip_concave) {
	// A U shape whose arms leave the box through its top: Sutherland-Hodgman
	// joins the arms with a zero-width bridge along the box edge
	const char *u = "MULTIPOLYGON(((0 0,0 10,2 10,2 2,8 2,8 10,10 10,10 0,0 0)))";
	checkClip(u, Box(Point(-1, -1), Point(11, 5)), 1, 32);
	// Cutting the base off leaves the two arms as separate polygons
	checkClip(u, Box(Point(-1, 4), Point(11, 12)), 2, 24);
}

MU_TEST(test_rect_clip_holes) {
	const char *holes = "MULTIPOLYGON(((0 0,0 10,10 10,10 0,0 0),(1 1,3 1,3 3,1 3,1 1),(6 4,9 4,9 6,6 6,6 4)))";
	// One hole inside the box, one cut by its right edge
	checkClip(holes, Box(Point(-1, -1), Point(7, 7)), 1, 43);
	// Box wholly inside the polygon: the box itself
	checkClip(holes, Box(Point(4, 7), Point(5, 8)), 1, 1);
	// Box wholly inside a hole: nothing
	checkClip(holes, Box(Point(1.5, 1.5), Point(2.5, 2.5)), 0, 0);
}

MU_TEST(test_rect_clip_trivial) {
	const char *square = "MULTIPOLYGON(((0 0,0 10,10 10,10 0,0 0)))";
	// Disjoint
	checkClip(square, Box(Point(20, 20), Point(30, 30)), 0, 0);
	// Polygon wholly inside the box is returned as it is
	MultiPolygon mp = fromWkt(square), clipped;
	mu_check(rect_clip(mp, Box(Point(-1, -1), Point(11, 11)), clipped));
	mu_check(geom::equals(mp, clipped));
}

MU_TEST(test_rect_clip_vertex_on_edge) {
	// The ring crosses the box edge at a (duplicated) vertex exactly on it.
	// (Boost's intersection returns nothing for the first box.)
	const char *poly = "MULTIPOLYGON(((0 0,0 4,0 4,0 10,10 10,10 6,12 4,10 2,10 0,0 0)))";
	checkClip(poly, Box(Point(-1, 4), Point(11, 11)), 1, 61.5);
	checkClip(poly, Box(Point(-1, -1), Point(11, 11)), 1, 103);
}

MU_TEST(test_rect_clip_touch_from_inside) {
	// A notch from the top comes down to touch the box's bottom edge at (5 0), then
	// goes back up. The box walk must not run through the touch: two polygons.
	const char *notch = "MULTIPOLYGON(((-5 -5,-5 8,4 8,5 0,6 8,15 8,15 -5,-5 -5)))";
	checkClip(notch, Box(Point(0, 0), Point(10, 10)), 2, 72);
	// The same touching the left edge
	const char *sideNotch = "MULTIPOLYGON(((-5 -5,-5 15,8 15,8 6,0 5,8 4,8 -5,-5 -5)))";
	checkClip(sideNotch, Box(Point(0, 0), Point(10, 10)), 2, 72);
}

MU_TEST_SUITE(test_suite_geom) {
	MU_RUN_TEST(test_rect_clip_concave);
	MU_RUN_TEST(test_rect_clip_holes);
	MU_RUN_TEST(test_rect_clip_trivial);
	MU_RUN_TEST(test_rect_clip_vertex_on_edge);
	MU_RUN_TEST(test_rect_clip_touch_from_inside);
}

int main() {
	MU_RUN_SUITE(test_suite_geom);
	MU_REPORT();
	return MU_EXIT_CODE;
}
