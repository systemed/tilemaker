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

// The precheck may only answer true when is_valid does
static void checkPrecheck(const MultiPolygon &mp, bool expected) {
	const bool pre = multiPolygonDefinitelyValid(mp);
	mu_check(pre == expected);
	mu_check(!pre || geom::is_valid(mp));
}

MU_TEST(test_precheck_overlapping_bboxes) {
	// An L and a square in its corner: bboxes overlap, interiors don't
	checkPrecheck(fromWkt("MULTIPOLYGON(((0 0,0 10,2 10,2 2,10 2,10 0,0 0)),((4 4,4 8,8 8,8 4,4 4)))"), true);
	// A polygon inside another's hole
	checkPrecheck(fromWkt("MULTIPOLYGON(((0 0,0 10,10 10,10 0,0 0),(2 2,8 2,8 8,2 8,2 2)),((4 4,4 6,6 6,6 4,4 4)))"), true);
	// A polygon inside another's interior
	checkPrecheck(fromWkt("MULTIPOLYGON(((0 0,0 10,10 10,10 0,0 0)),((4 4,4 6,6 6,6 4,4 4)))"), false);
	// Crossing polygons
	checkPrecheck(fromWkt("MULTIPOLYGON(((0 0,0 4,4 4,4 0,0 0)),((2 2,2 6,6 6,6 2,2 2)))"), false);
	// Touching at a vertex: valid, but too close to call
	checkPrecheck(fromWkt("MULTIPOLYGON(((0 0,0 10,2 10,2 2,10 2,10 0,0 0)),((2 2,4 8,8 8,8 4,2 2)))"), false);
}

MU_TEST(test_precheck_many_holes) {
	// 400 holes on a grid inside one square
	Polygon p;
	geom::read_wkt("POLYGON((0 0,0 100,100 100,100 0,0 0))", p);
	for (int i = 0; i < 20; i++)
		for (int j = 0; j < 20; j++) {
			Ring hole;
			const double x = 1 + i * 5, y = 1 + j * 5;
			geom::append(hole, Point(x, y)); geom::append(hole, Point(x + 2, y));
			geom::append(hole, Point(x + 2, y + 2)); geom::append(hole, Point(x, y + 2));
			geom::append(hole, Point(x, y));
			p.inners().push_back(hole);
		}
	MultiPolygon mp;
	mp.push_back(p);
	geom::correct(mp);
	checkPrecheck(mp, true);
	// Nest one hole inside another
	Ring nested;
	geom::append(nested, Point(1.5, 1.5)); geom::append(nested, Point(2.5, 1.5));
	geom::append(nested, Point(2.5, 2.5)); geom::append(nested, Point(1.5, 2.5));
	geom::append(nested, Point(1.5, 1.5));
	mp[0].inners().push_back(nested);
	geom::correct(mp);
	checkPrecheck(mp, false);
}

static Ring squareRing(double x, double y, double size) {
	Ring r;
	geom::append(r, Point(x, y)); geom::append(r, Point(x + size, y));
	geom::append(r, Point(x + size, y + size)); geom::append(r, Point(x, y + size));
	geom::append(r, Point(x, y));
	return r;
}

MU_TEST(test_precheck_large_polygon) {
	// A 40000-vertex outer ring (a circle) with 3600 holes: far more than walking the
	// outer ring for every hole would allow (the old 2^26 work cap)
	Polygon p;
	const int n = 40000;
	for (int i = 0; i <= n; i++) {
		const double a = 2 * M_PI * (i % n) / n;
		geom::append(p.outer(), Point(100 + 100 * std::cos(a), 100 + 100 * std::sin(a)));
	}
	for (int i = 0; i < 60; i++)
		for (int j = 0; j < 60; j++)
			p.inners().push_back(squareRing(40 + i * 2, 40 + j * 2, 1));
	MultiPolygon mp;
	mp.push_back(p);
	geom::correct(mp);
	checkPrecheck(mp, true);

	// A hole outside the outer ring
	MultiPolygon outside = mp;
	outside[0].inners().push_back(squareRing(300, 100, 1));
	geom::correct(outside);
	checkPrecheck(outside, false);

	// A hole nested in a hole on the other side of the polygon
	MultiPolygon nested = mp;
	nested[0].inners().insert(nested[0].inners().begin(), squareRing(40.25, 40.25, 0.5));
	geom::correct(nested);
	checkPrecheck(nested, false);
}

MU_TEST_SUITE(test_suite_geom) {
	MU_RUN_TEST(test_rect_clip_concave);
	MU_RUN_TEST(test_rect_clip_holes);
	MU_RUN_TEST(test_rect_clip_trivial);
	MU_RUN_TEST(test_rect_clip_vertex_on_edge);
	MU_RUN_TEST(test_rect_clip_touch_from_inside);
	MU_RUN_TEST(test_precheck_overlapping_bboxes);
	MU_RUN_TEST(test_precheck_many_holes);
	MU_RUN_TEST(test_precheck_large_polygon);
}

int main() {
	MU_RUN_SUITE(test_suite_geom);
	MU_REPORT();
	return MU_EXIT_CODE;
}
