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

static bool sameBox(const Box &a, const Box &b) {
	return geom::equals(a.min_corner(), b.min_corner()) && geom::equals(a.max_corner(), b.max_corner());
}

MU_TEST(test_merge_nearby_boxes) {
	// Far apart (e.g. islands either side of the antimeridian): kept separate
	std::vector<Box> far = merge_nearby_boxes({
		Box(Point(0, 0), Point(10, 10)),
		Box(Point(1000, 0), Point(1010, 10))
	}, 4);
	mu_check(far.size() == 2);

	// Within the gap: merged into their envelope
	std::vector<Box> near = merge_nearby_boxes({
		Box(Point(0, 0), Point(10, 10)),
		Box(Point(13, 0), Point(20, 5))
	}, 4);
	mu_check(near.size() == 1);
	mu_check(sameBox(near[0], Box(Point(0, 0), Point(20, 10))));

	// A and C are only close once B has joined A, after C was passed over
	std::vector<Box> chain = merge_nearby_boxes({
		Box(Point(0, 0), Point(10, 10)),
		Box(Point(30, 0), Point(40, 10)),
		Box(Point(12, 0), Point(27, 10))
	}, 4);
	mu_check(chain.size() == 1);
	mu_check(sameBox(chain[0], Box(Point(0, 0), Point(40, 10))));
}

MU_TEST(test_multipolygon_index_boxes) {
	// One polygon, or outers close together: the envelope
	MultiPolygon one = fromWkt("MULTIPOLYGON(((0 0,0 1,1 1,1 0,0 0)))");
	mu_check(multipolygon_index_boxes(one, 0.05).size() == 1);
	MultiPolygon close = fromWkt("MULTIPOLYGON(((0 0,0 1,1 1,1 0,0 0)),((1.5 0,1.5 1,2.5 1,2.5 0,1.5 0)))");
	std::vector<Box> closeBoxes = multipolygon_index_boxes(close, 0.05);
	mu_check(closeBoxes.size() == 1);
	mu_check(sameBox(closeBoxes[0], Box(Point(0, 0), Point(2.5, 1))));

	// Islands on both sides of the antimeridian: one box each, not one
	// spanning every longitude
	MultiPolygon aleutians = fromWkt("MULTIPOLYGON(((-179.5 52,-179.5 52.5,-179 52.5,-179 52,-179.5 52)),"
	                                 "((179 52,179 52.5,179.5 52.5,179.5 52,179 52)))");
	std::vector<Box> boxes = multipolygon_index_boxes(aleutians, 0.05);
	mu_check(boxes.size() == 2);
	for (const Box &b : boxes)
		mu_check(b.max_corner().x() - b.min_corner().x() < 1);
}

MU_TEST(test_simplify_no_holes) {
	// A near-collinear vertex goes; the corners, on the envelope, stay
	MultiPolygon mp = fromWkt("MULTIPOLYGON(((0 0,0 100,100 100,100 0,50 1,0 0)))");
	Polygon p = simplify(mp[0], 5);
	mu_check(p.outer().size() == 5);
	mu_check(p.inners().empty());
	mu_check(geom::is_valid(p));
}

MU_TEST(test_simplify_hole_near_outer) {
	// The outer ring has a spike up to (50 8). Dropping the hole's vertex at
	// (50 12) would take the hole's edge to y=5, across the spike, so the
	// hole must be checked against the outer ring and keep that vertex.
	MultiPolygon mp = fromWkt("MULTIPOLYGON(((0 0,0 100,100 100,100 0,55 0,50 8,45 0,0 0),(30 5,50 12,70 5,50 20,30 5)))");
	mu_check(geom::is_valid(mp));
	Polygon p = simplify(mp[0], 8);
	mu_check(p.inners().size() == 1);
	mu_check(p.inners()[0].size() == 5);
	mu_check(geom::is_valid(p));
}

// IndexedMultiPolygon::intersects must agree with geom::intersects. The
// multipolygon: a square with a square hole, an island in the hole, and a
// separate triangle; queries on, in, across and around every part.
MU_TEST(test_indexed_multipolygon) {
	MultiPolygon mp = fromWkt("MULTIPOLYGON(((0 0,0 10,10 10,10 0,0 0),(3 3,7 3,7 7,3 7,3 3)),"
	                          "((4 4,4 6,6 6,6 4,4 4)),((20 0,25 8,30 0,20 0)))");
	mu_check(geom::is_valid(mp));
	IndexedMultiPolygon index(mp);

	// Edge cases, checked by hand
	mu_check(index.intersects(Point(1, 1)));       // inside
	mu_check(!index.intersects(Point(3.5, 3.5)));  // in the hole
	mu_check(index.intersects(Point(5, 5)));       // on the island
	mu_check(index.intersects(Point(0, 5)));       // on the boundary
	mu_check(index.intersects(Point(3, 5)));       // on the hole's boundary
	mu_check(!index.intersects(Point(-1, 0)));     // outside, level with a vertex
	mu_check(index.intersects(Point(1, 0)));       // on an edge, level with vertices
	Linestring inHole; geom::read_wkt("LINESTRING(3.2 3.2,3.8 3.8)", inHole);
	mu_check(!index.intersects(inHole));
	Linestring touching; geom::read_wkt("LINESTRING(-1 10,0 10)", touching);
	mu_check(index.intersects(touching));
	Polygon around; geom::read_wkt("POLYGON((-5 -5,-5 50,50 50,50 -5,-5 -5))", around);
	mu_check(index.intersects(around));            // contains everything
	Polygon aroundIsland; geom::read_wkt("POLYGON((3.5 3.5,3.5 6.5,6.5 6.5,6.5 3.5,3.5 3.5))", aroundIsland);
	mu_check(index.intersects(aroundIsland));      // in the hole, around the island
	mu_check(index.intersects(Box(Point(1, 1), Point(2, 2))));   // box inside
	mu_check(!index.intersects(Box(Point(11, 1), Point(19, 2)))); // box between

	// Nested shells (invalid, but common in OSM, e.g. Cleveland National
	// Forest): a point in two polygons at once is inside, as geom::intersects says
	MultiPolygon nested;
	geom::read_wkt("MULTIPOLYGON(((0 0,0 10,10 10,10 0,0 0)),((2 2,2 8,8 8,8 2,2 2)))", nested);
	IndexedMultiPolygon nestedIndex(nested);
	mu_check(geom::intersects(Point(5, 5), nested));
	mu_check(nestedIndex.intersects(Point(5, 5)));
	Linestring inBoth; geom::read_wkt("LINESTRING(4 4,6 6)", inBoth);
	mu_check(nestedIndex.intersects(inBoth) == geom::intersects(inBoth, nested));

	// Many queries, against geom::intersects
	unsigned seed = 1;
	auto rnd = [&](double lo, double hi) {
		seed = seed * 1103515245u + 12345u;
		return lo + (hi - lo) * ((seed >> 8) & 0xffff) / 65535.0;
	};
	for (int i = 0; i < 2000; i++) {
		const Point p(rnd(-2, 32), rnd(-2, 12));
		mu_check(index.intersects(p) == geom::intersects(p, mp));

		Linestring ls;
		ls.push_back(p);
		ls.push_back(Point(p.x() + rnd(-3, 3), p.y() + rnd(-3, 3)));
		ls.push_back(Point(p.x() + rnd(-3, 3), p.y() + rnd(-3, 3)));
		mu_check(index.intersects(ls) == geom::intersects(ls, mp));

		const double w = rnd(0, 4), h = rnd(0, 4);
		const Box box(p, Point(p.x() + w, p.y() + h));
		mu_check(index.intersects(box) == geom::intersects(box, mp));

		Polygon tri;
		geom::append(tri.outer(), p);
		geom::append(tri.outer(), Point(p.x() + w, p.y()));
		geom::append(tri.outer(), Point(p.x(), p.y() + h));
		geom::append(tri.outer(), p);
		geom::correct(tri);
		mu_check(index.intersects(tri) == geom::intersects(tri, mp));
	}
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
	MU_RUN_TEST(test_merge_nearby_boxes);
	MU_RUN_TEST(test_multipolygon_index_boxes);
	MU_RUN_TEST(test_simplify_no_holes);
	MU_RUN_TEST(test_simplify_hole_near_outer);
	MU_RUN_TEST(test_indexed_multipolygon);
}

int main() {
	MU_RUN_SUITE(test_suite_geom);
	MU_REPORT();
	return MU_EXIT_CODE;
}
