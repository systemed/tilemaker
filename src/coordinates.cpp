#include "coordinates.h"
#include <math.h>
using namespace std;
namespace geom = boost::geometry;

double deg2rad(double deg) { return (M_PI/180.0) * deg; }
double rad2deg(double rad) { return (180.0/M_PI) * rad; }

// Project latitude (spherical Mercator)
// (if calling with raw coords, remember to divide/multiply by 10000000.0)
double lat2latp(double lat) { return rad2deg(log(tan(deg2rad(lat+90.0)/2.0))); }
double latp2lat(double latp) { return rad2deg(atan(exp(deg2rad(latp)))*2.0)-90.0; }

// Tile conversions
double lon2tilexf(double lon, uint z) { return scalbn((lon+180.0) * (1/360.0), (int)z); }
double latp2tileyf(double latp, uint z) { return scalbn((180.0-latp) * (1/360.0), (int)z); }
double lat2tileyf(double lat, uint z) { return latp2tileyf(lat2latp(lat), z); }
uint lon2tilex(double lon, uint z) { return lon2tilexf(lon, z); }
uint latp2tiley(double latp, uint z) { return latp2tileyf(latp, z); }
uint lat2tiley(double lat, uint z) { return lat2tileyf(lat, z); }
double tilex2lon(uint x, uint z) { return scalbn(x, -(int)z) * 360.0 - 180.0; }
double tiley2latp(uint y, uint z) { return 180.0 - scalbn(y, -(int)z) * 360.0; }
double tiley2lat(uint y, uint z) { return latp2lat(tiley2latp(y, z)); }

// Get a tile index
uint64_t latpLon2index(LatpLon ll, uint baseZoom) {
	return lon2tilex(ll.lon /10000000.0, baseZoom) * 4294967296 +
	       latp2tiley(ll.latp/10000000.0, baseZoom);
}

// Convert to actual length
double degp2meter(double degp, double latp) {
	return RadiusMeter * deg2rad(degp) * cos(deg2rad(latp2lat(latp)));
}
double meter2degp(double meter, double latp) {
	return rad2deg((1/RadiusMeter) * (meter / cos(deg2rad(latp2lat(latp)))));
}

// the range between smallest y and largest y is filled, for each x
void fillCoveredTiles(unordered_set<uint64_t> &tileSet) {
	vector<uint64_t> tileList(tileSet.begin(), tileSet.end());
	sort(tileList.begin(), tileList.end());

	uint64_t prevX = 0, prevY = static_cast<uint64_t>(-2);
	for (uint64_t index: tileList) {
		uint64_t tileX = index >> 32, tileY = index & 4294967296;
		if (tileX == prevX) {
			// this loop has no effect at the first iteration
			for (uint64_t fillY = prevY+1; fillY < tileY; fillY++) {
				tileSet.insert((tileX << 32) + fillY);
			}
		}
		prevX = tileX, prevY = tileY;
	}
}


// ------------------------------------------------------
// Helper class for dealing with spherical Mercator tiles

TileBbox::TileBbox(uint64_t i, uint z) {
	index = i; zoom = z;
	tiley = index & 4294967295L;
	tilex = index >> 32;
	minLon = tilex2lon(tilex  ,zoom);
	minLat = tiley2lat(tiley+1,zoom);
	maxLon = tilex2lon(tilex+1,zoom);
	maxLat = tiley2lat(tiley  ,zoom);
	minLatp = lat2latp(minLat);
	maxLatp = lat2latp(maxLat);
	xmargin = (maxLon -minLon )/200.0;
	ymargin = (maxLatp-minLatp)/200.0;
	xscale  = (maxLon -minLon )/4096.0;
	yscale  = (maxLatp-minLatp)/4096.0;
	clippingBox = Box(geom::make<Point>(minLon-xmargin, minLatp-ymargin),
		              geom::make<Point>(maxLon+xmargin, maxLatp+ymargin));
}

pair<int,int> TileBbox::scaleLatpLon(double latp, double lon) {
	int x = (lon -   minLon) / xscale;
	int y = (maxLatp - latp) / yscale;
	return pair<int,int>(x,y);
}

