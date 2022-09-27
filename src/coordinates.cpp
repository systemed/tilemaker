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

TileBbox::TileBbox(TileCoordinates i, uint z, bool h) {
	zoom = z;
	index = i;
	hires = h;
	minLon = tilex2lon(i.x  ,zoom);
	minLat = tiley2lat(i.y+1,zoom);
	maxLon = tilex2lon(i.x+1,zoom);
	maxLat = tiley2lat(i.y  ,zoom);
	minLatp = lat2latp(minLat);
	maxLatp = lat2latp(maxLat);
	xmargin = (maxLon -minLon )/200.0;
	ymargin = (maxLatp-minLatp)/200.0;
	xscale  = (maxLon -minLon )/(hires ? 8192.0 : 4096.0);
	yscale  = (maxLatp-minLatp)/(hires ? 8192.0 : 4096.0);
	clippingBox = Box(geom::make<Point>(minLon-xmargin, minLatp-ymargin),
		              geom::make<Point>(maxLon+xmargin, maxLatp+ymargin));
}

pair<int,int> TileBbox::scaleLatpLon(double latp, double lon) const {
	int x = floor( (lon -   minLon) / xscale );
	int y = floor( (maxLatp - latp) / yscale );
	return pair<int,int>(x,y);
}

MultiPolygon TileBbox::scaleGeometry(MultiPolygon const &src) const {
	MultiPolygon dst;
	for(auto poly: src) {
		Polygon p;

		// Copy the outer ring
		Ring outer;
		std::vector<Point> points;
		int lastx=INT_MAX, lasty=INT_MAX;
		for(auto &i: poly.outer()) {
			auto scaled = scaleLatpLon(i.y(), i.x());
			Point pt(scaled.second, scaled.first);
			if (scaled.second!=lastx || scaled.first!=lasty) points.push_back(pt);
			lastx=scaled.second; lasty=scaled.first;
		}
		if (points.size()<4) continue;
		geom::append(outer,points);
		geom::append(p,outer);

		// Copy the inner rings
		int num_rings = 0;
		for(auto &r: poly.inners()) {
			Ring inner;
			points.clear();
			lastx=INT_MAX, lasty=INT_MAX;
			for(auto &i: r) {
				auto scaled = scaleLatpLon(i.y(), i.x());
				Point pt(scaled.second, scaled.first);
				if (scaled.second!=lastx || scaled.first!=lasty) points.push_back(pt);
				lastx=scaled.second; lasty=scaled.first;
			}
			if (points.size()<4) continue;
			geom::append(inner,points);
			num_rings++;
			geom::interior_rings(p).resize(num_rings);
			geom::append(p, inner, num_rings-1);
		}

		// Add to multipolygon
		dst.push_back(p);
	}
	return dst;
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

template<typename T>
void impl_insertIntermediateTiles(T const &points, uint baseZoom, std::unordered_set<TileCoordinates> &tileSet) {
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

void insertIntermediateTiles(Linestring const &points, uint baseZoom, std::unordered_set<TileCoordinates> &tileSet)
{
	impl_insertIntermediateTiles(points, baseZoom, tileSet);
}

void insertIntermediateTiles(Ring const &points, uint baseZoom, std::unordered_set<TileCoordinates> &tileSet)
{
	impl_insertIntermediateTiles(points, baseZoom, tileSet);
}
