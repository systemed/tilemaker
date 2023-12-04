/*! \file */ 
#ifndef _COORDINATES_H
#define _COORDINATES_H

// Lightweight types and functions for coordinates, for classes that don't
// need to pull in boost::geometry.
//
// Things that pull in boost::geometry should go in coordinates_geom.h

#include <cstdint>
#include <utility>
#include <vector>
#include <deque>
#include <unordered_set>

// A 36-bit integer can store all OSM node IDs; we represent this as 16 collections
// of 32-bit integers.
#define NODE_SHARDS 16
typedef uint32_t ShardedNodeID;
typedef uint64_t NodeID;
typedef uint64_t WayID;

typedef std::vector<WayID> WayVec;


#ifdef FAT_TILE_INDEX
// Supports up to z22
typedef uint32_t TileCoordinate;
typedef uint16_t Z6Offset;
#define TILE_COORDINATE_MAX UINT32_MAX
#else
// Supports up to z14
typedef uint16_t TileCoordinate;
typedef uint8_t Z6Offset;
#define TILE_COORDINATE_MAX UINT16_MAX
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
inline bool operator==(const LatpLon &a, const LatpLon &b) { return a.latp==b.latp && a.lon==b.lon; }
namespace std {
	/// Hashing function so we can use an unordered_set
	template<>
	struct hash<LatpLon> {
		size_t operator()(const LatpLon &ll) const {
			return std::hash<int32_t>()(ll.latp) ^ std::hash<int32_t>()(ll.lon);
		}
	};
}

typedef std::vector<LatpLon> LatpLonVec;
typedef std::deque<LatpLon> LatpLonDeque;

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
double lon2tilexf(double lon, uint8_t z);
double latp2tileyf(double latp, uint8_t z);
double lat2tileyf(double lat, uint8_t z);
uint32_t lon2tilex(double lon, uint8_t z);
uint32_t latp2tiley(double latp, uint8_t z);
uint32_t lat2tiley(double lat, uint8_t z);
double tilex2lon(uint32_t x, uint8_t z);
double tiley2latp(uint32_t y, uint8_t z);
double tiley2lat(uint32_t y, uint8_t z);

// Get a tile index
TileCoordinates latpLon2index(LatpLon ll, uint8_t baseZoom);

// Earth's (mean) radius
// http://nssdc.gsfc.nasa.gov/planetary/factsheet/earthfact.html
// http://mathworks.com/help/map/ref/earthradius.html
constexpr double RadiusMeter = 6371000;

// Convert to actual length
double degp2meter(double degp, double latp);

double meter2degp(double meter, double latp);

// the range between smallest y and largest y is filled, for each x
void fillCoveredTiles(std::unordered_set<TileCoordinates> &tileSet);

#endif //_COORDINATES_H

