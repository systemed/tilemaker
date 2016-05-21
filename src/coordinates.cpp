#include <math.h>

struct LatpLon {
	int32_t latp;
	int32_t lon;
};

double deg2rad(double deg) { return (M_PI/180.0) * deg; }
double rad2deg(double rad) { return (180.0/M_PI) * rad; }

// Project latitude (spherical Mercator)
// (if calling with raw coords, remember to divide/multiply by 10000000.0)
double lat2latp(double lat) { return rad2deg(log(tan(deg2rad(lat+90.0)/2.0))); }
double latp2lat(double latp) { return rad2deg(atan(exp(deg2rad(latp)))*2.0)-90.0; }

// Tile conversions
uint lon2tilex(double lon, uint z) { return scalbn((lon+180.0) * (1/360.0), (int)z); }
uint latp2tiley(double latp, uint z) { return scalbn((180.0-latp) * (1/360.0), (int)z); }
uint lat2tiley(double lat, uint z) { return latp2tiley(lat2latp(lat), z); }
double tilex2lon(uint x, uint z) { return scalbn(x, -(int)z) * 360.0 - 180.0; }
double tiley2latp(uint y, uint z) { return 180.0 - scalbn(y, -(int)z) * 360.0; }
double tiley2lat(uint y, uint z) { return latp2lat(tiley2latp(y, z)); }

// Get a tile index
uint32_t latpLon2index(LatpLon ll, uint baseZoom) {
	return lon2tilex(ll.lon /10000000.0, baseZoom) * 65536 +
	       latp2tiley(ll.latp/10000000.0, baseZoom);
}

// Earth's (mean) radius
// http://nssdc.gsfc.nasa.gov/planetary/factsheet/earthfact.html
// http://mathworks.com/help/map/ref/earthradius.html
constexpr double RadiusMeter = 6371000;

// Convert to actual length
double degp2meter(double degp, double latp) {
	return RadiusMeter * deg2rad(degp) * cos(deg2rad(latp2lat(latp)));
}
double meter2degp(double meter, double latp) {
	return rad2deg((1/RadiusMeter) * (meter / cos(deg2rad(latp2lat(latp)))));
}

// Add intermediate points so we don't skip tiles on long segments
void insertIntermediateTiles(unordered_set <uint32_t> *tlPtr, int numPoints, LatpLon startLL, LatpLon endLL, uint baseZoom) {
	numPoints *= 3;	// perhaps overkill, but why not
	int32_t dLon  = endLL.lon -startLL.lon ;
	int32_t dLatp = endLL.latp-startLL.latp;
	for (int i=1; i<numPoints-1; i++) {
		LatpLon ll = LatpLon { startLL.latp + dLatp/numPoints*i, 
		                       startLL.lon  + dLon /numPoints*i };
		tlPtr->insert(latpLon2index(ll,baseZoom));
	}
}


// ------------------------------------------------------
// Helper class for dealing with spherical Mercator tiles

class TileBbox { public:
	double minLon, maxLon, minLat, maxLat, minLatp, maxLatp;
	double xmargin, ymargin, xscale, yscale;
	uint index, zoom, tiley, tilex;
	Box clippingBox;

	TileBbox(uint i, uint z) {
		index = i; zoom = z;
		tiley = index & 65535;
		tilex = index >> 16;
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

	pair<int,int> scaleLatpLon(double latp, double lon) {
		int x = (lon -   minLon) / xscale;
		int y = (maxLatp - latp) / yscale;
		return pair<int,int>(x,y);
	}
};
