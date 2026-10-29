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

	// forEachSet on a wrapped set visits the same tiles as a full scan, in x-major, y-minor order
	{
		PreciseTileCoordinatesSet z3(3);
		z3.set(0, 0); z3.set(0, 5); z3.set(2, 1); z3.set(2, 7); z3.set(7, 3);
		LossyTileCoordinatesSet z5(5, z3);

		std::vector<std::pair<TileCoordinate, TileCoordinate>> expected, visited;
		for (TileCoordinate x = 0; x < 32; x++)
			for (TileCoordinate y = 0; y < 32; y++)
				if (z5.test(x, y)) expected.push_back({x, y});
		z5.forEachSet([&](TileCoordinate x, TileCoordinate y) { visited.push_back({x, y}); });

		mu_check(expected.size() == 5 * 16);
		mu_check(visited == expected);
	}
}

MU_TEST_SUITE(test_suite_tile_coordinates_set) {
	MU_RUN_TEST(test_tile_coordinates_set);
}

int main() {
	MU_RUN_SUITE(test_suite_tile_coordinates_set);
	MU_REPORT();
	return MU_EXIT_CODE;
}
