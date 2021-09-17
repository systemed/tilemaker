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
static inline double clamp(double value, double limit) { 
	return (value < -limit ? -limit : (value > limit ? limit : value));
}
double lat2latp(double lat) { return rad2deg(log(tan(deg2rad(clamp(lat,85.06)+90.0)/2.0))); }
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
	xmargin = (maxLon -minLon )/200.0;
	ymargin = (maxLatp-minLatp)/200.0;
	xscale  = (maxLon -minLon )/4096.0;
	yscale  = (maxLatp-minLatp)/4096.0;
	clippingBox = Box(geom::make<Point>(minLon-xmargin, minLatp-ymargin),
		              geom::make<Point>(maxLon+xmargin, maxLatp+ymargin));
}

pair<int,int> TileBbox::scaleLatpLon(double latp, double lon) const {
	int x = floor( (lon -   minLon) / xscale );
	int y = floor( (maxLatp - latp) / yscale );
	return pair<int,int>(x,y);
}

pair<double, double> TileBbox::floorLatpLon(double latp, double lon) const {
	auto p = scaleLatpLon(latp, lon);
	return std::make_pair( -(p.second * yscale - maxLatp), p.first * xscale + minLon);
}

Box TileBbox::getTileBox() const {
	double xmargin = (maxLon -minLon )/8192.0;
	double ymargin = (maxLatp-minLatp)/8192.0;
	return Box(geom::make<Point>(minLon+xmargin, minLatp+ymargin), geom::make<Point>(maxLon-xmargin, maxLatp-ymargin));
}

Box TileBbox::getExtendBox() const {
	return Box(
    	geom::make<Point>( minLon-(maxLon-minLon)*2.0, minLatp-(maxLatp-minLatp)*(8191.0/8192.0)), 
		geom::make<Point>( maxLon+(maxLon-minLon)*(8191.0/8192.0), maxLatp+(maxLatp-minLatp)*2.0));
}

MultiPolygon round_coordinates(TileBbox const &bbox, MultiPolygon const &mp) 
{
	MultiPolygon combined_mp;
	for(auto new_p: mp) {
		for(auto &i: new_p.outer()) {
			auto round_i = bbox.floorLatpLon(i.y(), i.x());
			i = Point(round_i.second, round_i.first);
		}

		for(auto &r: new_p.inners()) {
			for(auto &i: r) {
				auto round_i = bbox.floorLatpLon(i.y(), i.x());
				i = Point(round_i.second, round_i.first);
			}
		}

		boost::geometry::remove_spikes(new_p);
		simplify_combine(combined_mp, std::move(new_p));
	}
	return combined_mp;
}
