#include <iostream>
#include "external/minunit.h"
#include "tile_coordinates_set.h"

MU_TEST(test_tile_coordinates_set) {
	{
		PreciseTileCoordinatesSet z0(0);
		mu_check(z0.test(0, 0) == false);
		mu_check(z0.size() == 0);
		mu_check(z0.zoom() == 0);

		z0.set(0, 0);
		mu_check(z0.test(0, 0) == true);
		mu_check(z0.size() == 1);
	}

	{
		PreciseTileCoordinatesSet z6(6);
		mu_check(z6.test(0, 0) == false);

		z6.set(0, 0);
		mu_check(z6.test(0, 0) == true);
		mu_check(z6.test(1, 0) == false);
		mu_check(z6.test(0, 1) == false);
		mu_check(z6.size() == 1);
		mu_check(z6.zoom() == 6);
	}

	// Wrapped sets should extrapolate from lower zooms
	{
		PreciseTileCoordinatesSet z1(1);
		LossyTileCoordinatesSet z2(2, z1);

		mu_check(z2.size() == 0);
		for (int x = 0; x < 4; x++) {
			for (int y = 0; y < 4; y++) {
				mu_check(z2.test(x, y) == false);
			}
		}
		z1.set(0, 0);
		mu_check(z2.size() == 4);
		mu_check(z2.test(0, 0) == true);
		mu_check(z2.test(0, 1) == true);
		mu_check(z2.test(1, 0) == true);
		mu_check(z2.test(1, 1) == true);
		mu_check(z2.test(2, 2) == false);
	}
}

static bool sameTiles(const TileCoordinatesSet& a, const TileCoordinatesSet& b) {
	std::vector<std::pair<TileCoordinate, TileCoordinate>> ta, tb;
	a.forEachSet([&](TileCoordinate x, TileCoordinate y) { ta.emplace_back(x, y); });
	b.forEachSet([&](TileCoordinate x, TileCoordinate y) { tb.emplace_back(x, y); });
	return ta == tb && a.size() == b.size();
}

MU_TEST(test_tile_coordinates_set_box) {
	// setBox must set exactly the tiles that set() would, including boxes that
	// start and end mid-word, span several words, or are clipped at the edge
	const TileCoordinate boxes[][4] = {
		{ 0, 0, 0, 0 }, { 3, 9, 5, 70 }, { 100, 101, 0, 1023 }, { 7, 7, 60, 200 },
		{ 1000, 1023, 990, 1023 }, { 1020, 1023, 1000, 1023 }
	};
	for (const auto& b : boxes) {
		PreciseTileCoordinatesSet bySet(10), byBox(10);
		for (TileCoordinate x = b[0]; x <= b[1]; x++)
			for (TileCoordinate y = b[2]; y <= b[3]; y++)
				bySet.set(x, y);
		byBox.setBox(b[0], b[1], b[2], b[3]);
		mu_check(sameTiles(bySet, byBox));
	}
	// Partly outside the 1024x1024 grid: the part inside is set
	PreciseTileCoordinatesSet edge(10);
	edge.setBox(1020, 1030, 1022, 1030);
	mu_check(edge.size() == 4 * 2);
	mu_check(edge.test(1023, 1023));
}

MU_TEST(test_tile_coordinates_set_merge) {
	PreciseTileCoordinatesSet a(12), b(12), both(12);
	for (TileCoordinate i = 0; i < 4096; i += 37) {
		a.set(i, (i * 7) % 4096);
		both.set(i, (i * 7) % 4096);
	}
	for (TileCoordinate i = 0; i < 4096; i += 53) {
		b.set((i * 3) % 4096, i);
		both.set((i * 3) % 4096, i);
	}
	a.merge(b);
	mu_check(sameTiles(a, both));
}

MU_TEST_SUITE(test_suite_tile_coordinates_set) {
	MU_RUN_TEST(test_tile_coordinates_set);
	MU_RUN_TEST(test_tile_coordinates_set_box);
	MU_RUN_TEST(test_tile_coordinates_set_merge);
}

int main() {
	MU_RUN_SUITE(test_suite_tile_coordinates_set);
	MU_REPORT();
	return MU_EXIT_CODE;
}
