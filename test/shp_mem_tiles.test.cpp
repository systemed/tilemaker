#include <iostream>
#include "external/minunit.h"
#include "shp_mem_tiles.h"

bool verbose = false;

namespace {
	MultiPolygon square(double x, double y, double size) {
		MultiPolygon mp;
		Polygon p;
		geom::append(p.outer(), Point(x, y));
		geom::append(p.outer(), Point(x, y + size));
		geom::append(p.outer(), Point(x + size, y + size));
		geom::append(p.outer(), Point(x + size, y));
		geom::append(p.outer(), Point(x, y));
		mp.push_back(p);
		return mp;
	}

	Box box(double x1, double y1, double x2, double y2) {
		return Box(Point(x1, y1), Point(x2, y2));
	}
}

MU_TEST(test_may_intersect) {
	Declutter declutter;
	ShpMemTiles shp(1, 14, declutter);
	shp.CreateNamedLayerIndex("layer");

	// Two small squares at opposite corners: the shape's bounding box, (0,0)-(10,10),
	// is almost all empty, like that of a country with far-flung islands.
	MultiPolygon mp = square(0, 0, 1);
	mp.push_back(square(9, 9, 1)[0]);
	shp.StoreGeometry(0, "layer", POLYGON_, mp, true, true, "corners", 0, 0, 0);

	// Outside the shape's bounding box
	mu_check(!shp.mayIntersect("layer", box(20, 20, 21, 21)));

	// A small box (a few z15 tiles) in the empty middle: checked tile by tile
	mu_check(!shp.mayIntersect("layer", box(5, 5, 5.001, 5.001)));
	// A large box (tens of thousands of z15 tiles) in the empty middle: checked at once
	mu_check(!shp.mayIntersect("layer", box(3, 3, 7, 7)));

	// Small and large boxes that touch a square
	mu_check(shp.mayIntersect("layer", box(0.5, 0.5, 0.501, 0.501)));
	mu_check(shp.mayIntersect("layer", box(0.5, 0.5, 5, 5)));
	mu_check(shp.mayIntersect("layer", box(5, 5, 9.5, 9.5)));

	// A large box that surrounds a square without containing a vertex of it
	mu_check(shp.mayIntersect("layer", box(-1, -1, 2, 2)));

	// Answers stay the same once tiles have been checked and cached
	mu_check(!shp.mayIntersect("layer", box(5, 5, 5.001, 5.001)));
	mu_check(!shp.mayIntersect("layer", box(3, 3, 7, 7)));
	mu_check(shp.mayIntersect("layer", box(0.5, 0.5, 0.501, 0.501)));
}

MU_TEST_SUITE(test_suite_shp_mem_tiles) {
	MU_RUN_TEST(test_may_intersect);
}

int main() {
	MU_RUN_SUITE(test_suite_shp_mem_tiles);
	MU_REPORT();
	return MU_EXIT_CODE;
}
