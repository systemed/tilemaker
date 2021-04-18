/*! \file */ 
#ifndef _COORDINATES_H
#define _COORDINATES_H

#include <iostream>
#include "geomtypes.h"
#include <utility>
#include <unordered_set>

#ifdef FAT_TILE_INDEX
typedef uint32_t TileCoordinate;
#else
typedef uint16_t TileCoordinate;
#endif
class TileCoordinates_ {

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
struct TileCoordinatesCompare {
    bool operator()(const class TileCoordinates_& a, const class TileCoordinates_& b) const {
		if(a.x > b.x)
			return false;
		if(a.x < b.x)
			return true;
        return a.y < b.y;
    }
};
typedef class TileCoordinates_ TileCoordinates;
namespace std {
	template<> struct hash<TileCoordinates> {
		size_t operator()(const TileCoordinates & obj) const {
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
		// Line is from p1 to p2
		Point p1 = p2;
		p2 = *it;

		// Calculate p2 tile, and mark it
		double tileXf2 = lon2tilexf(p2.x(), baseZoom), tileYf2 = latp2tileyf(p2.y(), baseZoom);
		TileCoordinate tileX2 = static_cast<TileCoordinate>(tileXf2), tileY2 = static_cast<TileCoordinate>(tileYf2);
		tileSet.insert(TileCoordinates(tileX2, tileY2));
		if (it == points.begin()) continue;	// first point, so no line

		// Calculate p1 tile
		double tileXf1 = lon2tilexf(p1.x(), baseZoom), tileYf1 = latp2tileyf(p1.y(), baseZoom);
		TileCoordinate tileX1 = static_cast<TileCoordinate>(tileXf1), tileY1 = static_cast<TileCoordinate>(tileYf1);
		tileSet.insert(TileCoordinates(tileX1,tileY1));

		// Supercover line algorithm from http://eugen.dedu.free.fr/projects/bresenham/
		int i;                       // loop counter
		int ystep, xstep;            // the step on y and x axis
		int error;                   // the error accumulated during the increment
		int errorprev;               // *vision the previous value of the error variable
		int y = tileY1, x = tileX1;  // the line points
		int ddy, ddx;                // compulsory variables: the double values of dy and dx
		int dx = tileX2 - tileX1;
		int dy = tileY2 - tileY1;

		if (dy < 0) { ystep = -1; dy = -dy; } else { ystep = 1; }
		if (dx < 0) { xstep = -1; dx = -dx; } else { xstep = 1; }

		ddy = 2 * dy;  // work with double values for full precision
		ddx = 2 * dx;
		if (ddx >= ddy) {  // first octant (0 <= slope <= 1)
			// compulsory initialization (even for errorprev, needed when dx==dy)
			errorprev = error = dx;  // start in the middle of the square
			for (i=0 ; i < dx ; i++) {  // do not use the first point (already done)
				x += xstep;
				error += ddy;
				if (error > ddx){  // increment y if AFTER the middle ( > )
					y += ystep;
					error -= ddx;
					// three cases (octant == right->right-top for directions below):
					if (error + errorprev < ddx)  // bottom square also
						tileSet.insert(TileCoordinates(x, y-ystep));
					else if (error + errorprev > ddx)  // left square also
						tileSet.insert(TileCoordinates(x-xstep, y));
					else {  // corner: bottom and left squares also
						tileSet.insert(TileCoordinates(x, y-ystep));
						tileSet.insert(TileCoordinates(x-xstep, y));
					}
				}
				tileSet.insert(TileCoordinates(x, y));
				errorprev = error;
			}
		} else {  // the same as above
			errorprev = error = dy;
			for (i=0 ; i < dy ; i++){
				y += ystep;
				error += ddx;
				if (error > ddy){
					x += xstep;
					error -= ddy;
					if (error + errorprev < ddy)
						tileSet.insert(TileCoordinates(x-xstep, y));
					else if (error + errorprev > ddy)
						tileSet.insert(TileCoordinates(x, y-ystep));
					else{
						tileSet.insert(TileCoordinates(x-xstep, y));
						tileSet.insert(TileCoordinates(x, y-ystep));
					}
				}
				tileSet.insert(TileCoordinates(x, y));
				errorprev = error;
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
	Box getTileBox() const;
};

#endif //_COORDINATES_H

