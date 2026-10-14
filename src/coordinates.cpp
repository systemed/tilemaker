#include "coordinates.h"
#include <math.h>
#include <algorithm>

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
