/*! \file */ 
#ifndef _COORDINATES_H
#define _COORDINATES_H

#include "geomtypes.h"
#include <utility>
#include <unordered_set>

#ifdef COMPACT_TILE_INDEX
typedef uint16_t TileCoordinate;
#else
typedef uint32_t TileCoordinate;
#endif
class TileCoordinates_
{
public:
	TileCoordinate x, y;

	TileCoordinates_();
	TileCoordinates_(TileCoordinate x, TileCoordinate y);

	bool operator ==(const TileCoordinates_ & obj) const
	{
		if (x != obj.x)
			return false;
		return y == obj.y;
	}
};
struct TileCoordinatesCompare
{
    bool operator()(const class TileCoordinates_& a, const class TileCoordinates_& b) const {
		if(a.x > b.x)
			return false;
		if(a.x < b.x)
			return true;
        return a.y < b.y;
    }
};
typedef class TileCoordinates_ TileCoordinates;
namespace std
{
	template<> struct hash<TileCoordinates>
	{
		size_t operator()(const TileCoordinates & obj) const
		{
			return hash<TileCoordinate>()(obj.x) ^ hash<TileCoordinate>()(obj.y);
		}
	};
}

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
TileCoordinates latpLon2index(LatpLon ll, uint baseZoom);

// Earth's (mean) radius
// http://nssdc.gsfc.nasa.gov/planetary/factsheet/earthfact.html
// http://mathworks.com/help/map/ref/earthradius.html
constexpr double RadiusMeter = 6371000;

// Convert to actual length
double degp2meter(double degp, double latp);

double meter2degp(double meter, double latp);

template<typename T>
void insertIntermediateTiles(const T &points, uint baseZoom, std::unordered_set<TileCoordinates> &tileSet) {
	Point p2(0, 0);
	for (auto it = points.begin(); it != points.end(); ++it) {
		Point p1 = p2;
		p2 = *it;

		double tileXf2 = lon2tilexf(p2.x(), baseZoom), tileYf2 = latp2tileyf(p2.y(), baseZoom);
		TileCoordinate tileX2 = static_cast<TileCoordinate>(tileXf2), tileY2 = static_cast<TileCoordinate>(tileYf2);

		// insert vertex
		tileSet.insert(TileCoordinates(tileX2, tileY2));
		// p1 is not available at the first iteration
		if (it == points.begin()) continue;

		double tileXf1 = lon2tilexf(p1.x(), baseZoom), tileYf1 = latp2tileyf(p1.y(), baseZoom);
		TileCoordinate tileX1 = static_cast<TileCoordinate>(tileXf1), tileY1 = static_cast<TileCoordinate>(tileYf1);
		double dx = tileXf2 - tileXf1, dy = tileYf2 - tileYf1;

		// insert all X border
		if (tileX1 != tileX2) {
			double slope = dy / dx;
			TileCoordinate tileXmin = std::min(tileX1, tileX2);
			TileCoordinate tileXmax = std::max(tileX1, tileX2);
			for (TileCoordinate tileXcur = tileXmin+1; tileXcur <= tileXmax; tileXcur++) {
				TileCoordinate tileYcur = static_cast<TileCoordinate>(tileYf1 + (static_cast<double>(tileXcur) - tileXf1) * slope);
				tileSet.insert(TileCoordinates(tileXcur, tileYcur));
			}
		}

		// insert all Y border
		if (tileY1 != tileY2) {
			double slope = dx / dy;
			TileCoordinate tileYmin = std::min(tileY1, tileY2);
			TileCoordinate tileYmax = std::max(tileY1, tileY2);
			for (TileCoordinate tileYcur = tileYmin+1; tileYcur <= tileYmax; tileYcur++) {
				TileCoordinate tileXcur = static_cast<TileCoordinate>(tileXf1 + (static_cast<double>(tileYcur) - tileYf1) * slope);
				tileSet.insert(TileCoordinates(tileXcur, tileYcur));
			}
		}
	}
}

// the range between smallest y and largest y is filled, for each x
void fillCoveredTiles(std::unordered_set<TileCoordinates> &tileSet);

// ------------------------------------------------------
// Helper class for dealing with spherical Mercator tiles

class TileBbox { 

public:
	double minLon, maxLon, minLat, maxLat, minLatp, maxLatp;
	double xmargin, ymargin, xscale, yscale;
	TileCoordinates index;
	uint zoom;
	Box clippingBox;

	TileBbox(TileCoordinates i, uint z);

	std::pair<int,int> scaleLatpLon(double latp, double lon) const;
};

#endif //_COORDINATES_H

