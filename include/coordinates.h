#ifndef _COORDINATES_H
#define _COORDINATES_H

#include "geomtypes.h"
#include <utility>
#include <unordered_set>

struct LatpLon {
	int32_t latp;
	int32_t lon;
};

double deg2rad(double deg);
double rad2deg(double rad);

// max/min latitudes
constexpr double MaxLat = 85.0511;
constexpr double MinLat = -MaxLat;

// Project latitude (spherical Mercator)
// (if calling with raw coords, remember to divide/multiply by 10000000.0)
double lat2latp(double lat);
double latp2lat(double latp);

// Tile conversions
double lon2tilexf(double lon, uint z);
double latp2tileyf(double latp, uint z);
double lat2tileyf(double lat, uint z);
uint lon2tilex(double lon, uint z);
uint latp2tiley(double latp, uint z);
uint lat2tiley(double lat, uint z);
double tilex2lon(uint x, uint z);
double tiley2latp(uint y, uint z);
double tiley2lat(uint y, uint z);

// Get a tile index
uint32_t latpLon2index(LatpLon ll, uint baseZoom);

// Earth's (mean) radius
// http://nssdc.gsfc.nasa.gov/planetary/factsheet/earthfact.html
// http://mathworks.com/help/map/ref/earthradius.html
constexpr double RadiusMeter = 6371000;

// Convert to actual length
double degp2meter(double degp, double latp);

double meter2degp(double meter, double latp);

template<typename T>
void insertIntermediateTiles(const T &points, uint baseZoom, std::unordered_set<uint32_t> &tileSet) {
	Point p2(0, 0);
	for (auto it = points.begin(); it != points.end(); ++it) {
		Point p1 = p2;
		p2 = *it;

		double tileXf2 = lon2tilexf(p2.x(), baseZoom), tileYf2 = latp2tileyf(p2.y(), baseZoom);
		uint tileX2 = static_cast<uint>(tileXf2), tileY2 = static_cast<uint>(tileYf2);

		// insert vertex
		tileSet.insert((tileX2 << 16) + tileY2);
		// p1 is not available at the first iteration
		if (it == points.begin()) continue;

		double tileXf1 = lon2tilexf(p1.x(), baseZoom), tileYf1 = latp2tileyf(p1.y(), baseZoom);
		uint tileX1 = static_cast<uint>(tileXf1), tileY1 = static_cast<uint>(tileYf1);
		double dx = tileXf2 - tileXf1, dy = tileYf2 - tileYf1;

		// insert all X border
		if (tileX1 != tileX2) {
			double slope = dy / dx;
			uint tileXmin = std::min(tileX1, tileX2);
			uint tileXmax = std::max(tileX1, tileX2);
			for (uint tileXcur = tileXmin+1; tileXcur <= tileXmax; tileXcur++) {
				uint tileYcur = static_cast<uint>(tileYf1 + (static_cast<double>(tileXcur) - tileXf1) * slope);
				tileSet.insert((tileXcur << 16) + tileYcur);
			}
		}

		// insert all Y border
		if (tileY1 != tileY2) {
			double slope = dx / dy;
			uint tileYmin = std::min(tileY1, tileY2);
			uint tileYmax = std::max(tileY1, tileY2);
			for (uint tileYcur = tileYmin+1; tileYcur <= tileYmax; tileYcur++) {
				uint tileXcur = static_cast<uint>(tileXf1 + (static_cast<double>(tileYcur) - tileYf1) * slope);
				tileSet.insert((tileXcur << 16) + tileYcur);
			}
		}
	}
}

// the range between smallest y and largest y is filled, for each x
void fillCoveredTiles(std::unordered_set<uint32_t> &tileSet);

// ------------------------------------------------------
// Helper class for dealing with spherical Mercator tiles

class TileBbox { 

public:
	double minLon, maxLon, minLat, maxLat, minLatp, maxLatp;
	double xmargin, ymargin, xscale, yscale;
	uint index, zoom, tiley, tilex;
	Box clippingBox;

	TileBbox(uint i, uint z);

	std::pair<int,int> scaleLatpLon(double latp, double lon) const;
};

#endif //_COORDINATES_H

