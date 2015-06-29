#include <math.h>

struct LatLon {
	int32_t lat;
	int32_t lon;
};

// Project latitude (spherical Mercator)
// (if calling with raw coords, remember to divide/multiply by 10000000.0)
inline double lat2latp(double lat) {
	return 180.0/M_PI * log(tan(M_PI/4.0+lat*(M_PI/180.0)/2.0));
}

// Tile conversions
int lon2tilex(double lon, uint z) { return (int)(floor((lon + 180.0) / 360.0 * pow(2.0, z)));  }
int lat2tiley(double lat, uint z) { return (int)(floor((1.0 - log( tan(lat * M_PI/180.0) + 1.0 / cos(lat * M_PI/180.0)) / M_PI) / 2.0 * pow(2.0, z))); }
double tilex2lon(int x, uint z) { return x / pow(2.0, z) * 360.0 - 180; }
double tiley2lat(int y, uint z) { double n = M_PI - 2.0 * M_PI * y / pow(2.0, z); return 180.0 / M_PI * atan(0.5 * (exp(n) - exp(-n))); }

// Get a tile index
inline uint32_t latLon2index(LatLon ll, uint baseZoom) {
	return lon2tilex(ll.lon/10000000.0, baseZoom) * 65536 + 
	       lat2tiley(ll.lat/10000000.0, baseZoom);
}

// Add intermediate points so we don't skip tiles on long segments
void insertIntermediateTiles(unordered_set <uint32_t> *tlPtr, int numPoints, LatLon startLL, LatLon endLL, uint baseZoom) {
	numPoints *= 3;	// perhaps overkill, but why not
	int32_t dLon = endLL.lon-startLL.lon;
	int32_t dLat = endLL.lat-startLL.lat;
	for (int i=1; i<numPoints-1; i++) {
		LatLon ll = LatLon { startLL.lat + dLat/numPoints*i, 
		                     startLL.lon + dLon/numPoints*i };
		tlPtr->insert(latLon2index(ll,baseZoom));
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
		xmargin = (maxLon-minLon)/200.0;
		ymargin = (maxLat-minLat)/200.0;
		xscale  = (maxLon-minLon)/4096.0;
		yscale  = (lat2latp(maxLat)-minLatp)/4096.0;
		clippingBox = Box(geom::make<Point>(minLon-xmargin, minLat-ymargin), 
			              geom::make<Point>(maxLon+xmargin, maxLat+ymargin));
	}

	pair<int,int> scaleLatLon(double lat, double lon) {
		int x = (lon - minLon) / xscale;
		int y = (maxLatp - lat2latp(lat)) / yscale;
		return pair<int,int>(x,y);
	}
};
