#include "coordinates.h"
#include <math.h>
using namespace std;
namespace geom = boost::geometry;

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
TileCoordinates latpLon2index(LatpLon ll, uint baseZoom) {
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
void fillCoveredTiles(unordered_set<TileCoordinates> &tileSet) {
	vector<TileCoordinates> tileList(tileSet.begin(), tileSet.end());
	sort(tileList.begin(), tileList.end(), TileCoordinatesCompare());

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


// ------------------------------------------------------
// Helper class for dealing with spherical Mercator tiles

TileBbox::TileBbox(TileCoordinates i, uint z) {
	zoom = z;
	index = i;
	minLon = tilex2lon(i.x  ,zoom);
	minLat = tiley2lat(i.y+1,zoom);
	maxLon = tilex2lon(i.x+1,zoom);
	maxLat = tiley2lat(i.y  ,zoom);
	minLatp = lat2latp(minLat);
	maxLatp = lat2latp(maxLat);
	//xmargin = (maxLon -minLon )/200.0;
	//ymargin = (maxLatp-minLatp)/200.0;
	xmargin = 0;
	ymargin = 0;
	xscale  = (maxLon -minLon )/4096.0;
	yscale  = (maxLatp-minLatp)/4096.0;
	clippingBox = Box(geom::make<Point>(minLon-xmargin, minLatp-ymargin),
		              geom::make<Point>(maxLon+xmargin, maxLatp+ymargin));
}

pair<int,int> TileBbox::scaleLatpLon(double latp, double lon) const {
	int x = (lon -   minLon) / xscale;
	int y = (maxLatp - latp) / yscale;
	return pair<int,int>(x,y);
}

