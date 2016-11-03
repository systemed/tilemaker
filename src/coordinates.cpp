#include <math.h>

struct LatpLon {
	int32_t latp;
	int32_t lon;
};

double deg2rad(double deg) { return (M_PI/180.0) * deg; }
double rad2deg(double rad) { return (180.0/M_PI) * rad; }

// max/min latitudes
constexpr double MaxLat = 85.0511;
constexpr double MinLat = -MaxLat;

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

template<typename T>
void insertIntermediateTiles(const T &points, uint baseZoom, unordered_set<uint32_t> &tileSet) {
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
			uint tileXmin = min(tileX1, tileX2);
			uint tileXmax = max(tileX1, tileX2);
			for (uint tileXcur = tileXmin+1; tileXcur <= tileXmax; tileXcur++) {
				uint tileYcur = static_cast<uint>(tileYf1 + (static_cast<double>(tileXcur) - tileXf1) * slope);
				tileSet.insert((tileXcur << 16) + tileYcur);
			}
		}

		// insert all Y border
		if (tileY1 != tileY2) {
			double slope = dx / dy;
			uint tileYmin = min(tileY1, tileY2);
			uint tileYmax = max(tileY1, tileY2);
			for (uint tileYcur = tileYmin+1; tileYcur <= tileYmax; tileYcur++) {
				uint tileXcur = static_cast<uint>(tileXf1 + (static_cast<double>(tileYcur) - tileYf1) * slope);
				tileSet.insert((tileXcur << 16) + tileYcur);
			}
		}
	}
}

// the range between smallest y and largest y is filled, for each x
void fillCoveredTiles(unordered_set<uint32_t> &tileSet) {
	vector<uint32_t> tileList(tileSet.begin(), tileSet.end());
	sort(tileList.begin(), tileList.end());

	uint prevX = 0, prevY = static_cast<uint>(-2);
	for (uint32_t index: tileList) {
		uint tileX = index >> 16, tileY = index & 65535;
		if (tileX == prevX) {
			// this loop has no effect at the first iteration
			for (uint fillY = prevY+1; fillY < tileY; fillY++) {
				tileSet.insert((tileX << 16) + fillY);
			}
		}
		prevX = tileX, prevY = tileY;
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
