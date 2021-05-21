/*! \file */ 
#ifndef _COORDINATES_H
#define _COORDINATES_H

#include <iostream>
#include "geom.h"
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

void insertIntermediateTiles(Linestring const &points, uint baseZoom, std::unordered_set<TileCoordinates> &tileSet);
void insertIntermediateTiles(Ring const &points, uint baseZoom, std::unordered_set<TileCoordinates> &tileSet);

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
	std::pair<double, double> floorLatpLon(double latp, double lon) const;

	Box getTileBox() const;
	Box getExtendBox() const;
};

// Round coordinates to integer coordinates of bbox
// TODO: This should be self-intersection aware!!
MultiPolygon round_coordinates(TileBbox const &bbox, MultiPolygon const &mp);

#endif //_COORDINATES_H

