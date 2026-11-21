#include "coordinates.h"
#include <math.h>
#include <algorithm>
#include <cmath>
#include <cstdint>

TileCoordinates_::TileCoordinates_() {
	this->x = 0;
	this->y = 0;
}

TileCoordinates_::TileCoordinates_(TileCoordinate x, TileCoordinate y) {
	this->x = x;
	this->y = y;
}

double deg2rad(double deg) { return (M_PI/180.0) * deg; }
double rad2deg(double rad) { return (180.0/M_PI) * rad; }

// Project latitude (spherical Mercator)
// (if calling with raw coords, remember to divide/multiply by 10000000.0)
static inline double clamp(double value, double limit) { 
	return (value < -limit ? -limit : (value > limit ? limit : value));
}
double lat2latp(double lat) { return rad2deg(log(tan(deg2rad(clamp(lat,85.06)+90.0)/2.0))); }
double latp2lat(double latp) { return rad2deg(atan(exp(deg2rad(latp)))*2.0)-90.0; }

// Fast exact int(lat2latp(lat / 1e7) * 1e7) for latitudes in 1e-7 degrees (the conversion of every
// node read from the .osm.pbf).
//
// A piecewise polynomial (degree 5, 1024 intervals over [0, 85.06] degrees, |lat|; the function is
// odd) approximates lat2latp(lat) * 1e7. Where it is more than Margin (1e-3, in 1e-7 degrees)
// away from an integer, the truncation of lat2latp's result is the same integer: the polynomial is
// within ~1e-5 of the exact function, lat2latp's double result within ~2e-6 (rounding of tan's
// argument near the clamp, log/tan within an ulp). Otherwise (0.2% of the latitudes, and beyond the
// clamp at +-85.06) lat2latp itself is called. Checked against lat2latp for every latitude up to +-86 degrees (and samples beyond).
namespace {
namespace latp_e7 {
	constexpr int N = 1024;
	constexpr double Limit = 85.06;	// lat2latp's clamp
	constexpr double Margin = 1e-3;
	constexpr uint32_t MaxFast = 850599999;	// |lat| up to this: below the clamp
	struct Table {
		double scale;	// intervals per degree
		double scalePerE7;	// intervals per 1e-7 degree
		double c[N][6];	// per interval, monomial coefficients in t in [-1, 1]
		Table() {
			scale = N / Limit;
			scalePerE7 = scale / 10000000.0;
			const double pi = 3.14159265358979323846;
			for (int i = 0; i < N; i++) {
				// Chebyshev interpolation at 6 nodes, then the monomial coefficients
				const double lo = i / scale, hi = (i + 1) / scale;
				double f[6], ch[6] = {};
				for (int j = 0; j < 6; j++) {
					const double t = std::cos(pi * (j + 0.5) / 6);
					f[j] = lat2latp((lo + hi) / 2 + t * (hi - lo) / 2) * 10000000.0;
				}
				for (int k = 0; k < 6; k++) {
					double s = 0;
					for (int j = 0; j < 6; j++) s += f[j] * std::cos(pi * k * (j + 0.5) / 6);
					ch[k] = s * (k == 0 ? 1.0 : 2.0) / 6;
				}
				// T0 = 1, T1 = t, T2 = 2t^2-1, T3 = 4t^3-3t, T4 = 8t^4-8t^2+1, T5 = 16t^5-20t^3+5t
				c[i][0] = ch[0] - ch[2] + ch[4];
				c[i][1] = ch[1] - 3 * ch[3] + 5 * ch[5];
				c[i][2] = 2 * ch[2] - 8 * ch[4];
				c[i][3] = 4 * ch[3] - 20 * ch[5];
				c[i][4] = 8 * ch[4];
				c[i][5] = 16 * ch[5];
			}
		}
	};
	inline const Table& table() { static const Table t; return t; }

	inline int32_t exact(int32_t lat) { return int(lat2latp(double(lat) / 10000000.0) * 10000000.0); }

	inline int32_t convert(int32_t lat) {
		const Table& tb = table();
		const uint32_t ua = lat < 0 ? 0u - uint32_t(lat) : uint32_t(lat);
		if (ua > MaxFast) return exact(lat);
		const double s = double(ua) * tb.scalePerE7;
		int i = int(s);
		if (i >= N) i = N - 1;
		const double t = 2 * (s - i) - 1;
		const double* c = tb.c[i];
		const double t2 = t * t;
		const double v = (c[0] + c[1] * t) + t2 * ((c[2] + c[3] * t) + t2 * (c[4] + c[5] * t));
		// the truncation of every value within Margin of v (v - Margin >= 0: truncation = floor)
		const double vlo = v - Margin;
		if (!(vlo >= 0)) return exact(lat);
		const int64_t lo = int64_t(vlo);
		if (lo != int64_t(v + Margin)) return exact(lat);
		return lat < 0 ? -int32_t(lo) : int32_t(lo);
	}
}
}

int32_t lat2latpE7(int32_t lat) { return latp_e7::convert(lat); }

// Tile conversions
double lon2tilexf(double lon, uint8_t z) { return scalbn((lon+180.0) * (1/360.0), (int)z); }
double latp2tileyf(double latp, uint8_t z) { return scalbn((180.0-latp) * (1/360.0), (int)z); }
double lat2tileyf(double lat, uint8_t z) { return latp2tileyf(lat2latp(lat), z); }
uint32_t lon2tilex(double lon, uint8_t z) { return lon2tilexf(lon, z); }
uint32_t latp2tiley(double latp, uint8_t z) { return latp2tileyf(latp, z); }
uint32_t lat2tiley(double lat, uint8_t z) { return lat2tileyf(lat, z); }
double tilex2lon(uint32_t x, uint8_t z) { return scalbn(x, -(int)z) * 360.0 - 180.0; }
double tiley2latp(uint32_t y, uint8_t z) { return 180.0 - scalbn(y, -(int)z) * 360.0; }
double tiley2lat(uint32_t y, uint8_t z) { return latp2lat(tiley2latp(y, z)); }

// Get a tile index
TileCoordinates latpLon2index(LatpLon ll, uint8_t baseZoom) {
	return TileCoordinates(lon2tilex(ll.lon /10000000.0, baseZoom),
	       latp2tiley(ll.latp/10000000.0, baseZoom));
}

// Convert to actual length
double degp2meter(double degp, double latp) {
	return RadiusMeter * deg2rad(degp) * cos(deg2rad(latp2lat(latp)));
}
double meter2degp(double meter, double latp) {
	return rad2deg((1/RadiusMeter) * (meter / cos(deg2rad(latp2lat(latp)))));
}

// the range between smallest y and largest y is filled, for each x
void fillCoveredTiles(std::unordered_set<TileCoordinates>& tileSet) {
	std::vector<TileCoordinates> tileList(tileSet.begin(), tileSet.end());
	std::sort(tileList.begin(), tileList.end(), TileCoordinatesCompare());

	TileCoordinate prevX = 0, prevY = static_cast<TileCoordinate>(-2);
	for (TileCoordinates index: tileList) {
		TileCoordinate tileX = index.x, tileY = index.y;
		if (tileX == prevX) {
			// this loop has no effect at the first iteration
			for (TileCoordinate fillY = prevY+1; fillY < tileY; fillY++) {
				tileSet.insert(TileCoordinates(tileX, fillY));
			}
		}
		prevX = tileX, prevY = tileY;
	}
}

void sortUniqueTiles(std::vector<TileCoordinates> &tiles, size_t start) {
	if (tiles.size() - start > 1) {
		std::sort(tiles.begin() + start, tiles.end(), TileCoordinatesCompare());
		tiles.erase(std::unique(tiles.begin() + start, tiles.end()), tiles.end());
	}
}

void fillCoveredTiles(std::vector<TileCoordinates> &tiles) {
	// Fast exit when no column has a y gap (the common case)
	size_t n = tiles.size(), i = 1;
	while (i < n && !(tiles[i].x == tiles[i-1].x && tiles[i].y > tiles[i-1].y + 1)) i++;
	if (i >= n) return;

	// Rebuild in order: copy the prefix, then each tile preceded by the y-gap tiles of its column
	thread_local std::vector<TileCoordinates> filled;
	filled.clear();
	filled.insert(filled.end(), tiles.begin(), tiles.begin() + i);
	for (; i < n; i++) {
		const TileCoordinates prev = tiles[i-1], cur = tiles[i];
		if (cur.x == prev.x) {
			for (TileCoordinate fillY = prev.y+1; fillY < cur.y; fillY++)
				filled.push_back(TileCoordinates(cur.x, fillY));
		}
		filled.push_back(cur);
	}
	tiles.swap(filled);
	if (filled.capacity() > (1u << 16)) std::vector<TileCoordinates>().swap(filled);
}
