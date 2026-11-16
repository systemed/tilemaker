#include "coordinates_geom.h"
#ifdef __SSE2__
#include <emmintrin.h>
#endif

using namespace std;
namespace geom = boost::geometry;

// ------------------------------------------------------
// Helper class for dealing with spherical Mercator tiles

TileBbox::TileBbox(TileCoordinates i, uint z, bool h, bool e) {
	zoom = z;
	index = i;
	hires = h;
	endZoom = e;
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

#ifdef __SSE2__
// scaleLatpLon() of a point (x lon, y latp), both lanes at once, as doubles (x low, y high).
// Same subtraction and division per lane; floor() by truncation, minus one where that rounded
// up. Returns false if a lane doesn't fit an int (the caller then uses scaleLatpLon(), whose
// out of range conversion this would not reproduce).
static inline bool scaleLatpLon2(__m128d v, __m128d hiMinuend, __m128d loSubtrahend, __m128d scale, __m128d &out) {
	// lanes: (lon - minLon, maxLatp - latp)
	const __m128d a = _mm_move_sd(hiMinuend, v);   // (lon, maxLatp)
	const __m128d b = _mm_move_sd(v, loSubtrahend); // (minLon, latp)
	const __m128d d = _mm_div_pd(_mm_sub_pd(a, b), scale);
	const __m128i t = _mm_cvttpd_epi32(d);           // truncated; INT_MIN if NaN or out of range
	if (_mm_movemask_epi8(_mm_cmpeq_epi32(t, _mm_set_epi32(0, 0, INT32_MIN, INT32_MIN))) & 0xff) return false;
	const __m128d up = _mm_cmpgt_pd(_mm_cvtepi32_pd(t), d);
	const __m128i f = _mm_add_epi32(t, _mm_shuffle_epi32(_mm_castpd_si128(up), _MM_SHUFFLE(3, 3, 2, 0)));
	out = _mm_cvtepi32_pd(f);
	return true;
}
#endif

void TileBbox::scaleLine(std::vector<std::pair<int,int>> &pts, Linestring const &src) const {
#ifdef __SSE2__
	// scale every point first (independent divisions), then drop repeats in place
	const size_t n = src.size();
	pts.resize(n);
	const __m128d hiMinuend = _mm_set_pd(maxLatp, 0), loSubtrahend = _mm_set_sd(minLon), scale = _mm_set_pd(yscale, xscale);
	for (size_t k = 0; k < n; k++) {
		const Point &i = src[k];
		__m128d sp;
		if (scaleLatpLon2(_mm_loadu_pd(reinterpret_cast<const double*>(&i)), hiMinuend, loSubtrahend, scale, sp)) {
			// sp holds two ints as doubles: converting back is exact
			const __m128i t = _mm_cvttpd_epi32(sp);
			pts[k].first = _mm_cvtsi128_si32(t);
			pts[k].second = _mm_cvtsi128_si32(_mm_shuffle_epi32(t, _MM_SHUFFLE(1, 1, 1, 1)));
		} else
			pts[k] = scaleLatpLon(i.y(), i.x());
	}
	size_t w = n ? 1 : 0;
	for (size_t r = 1; r < n; r++)
		if (pts[r] != pts[w - 1]) pts[w++] = pts[r];
	pts.resize(w);
#else
	pts.clear();
	for (auto const &i : src) {
		const std::pair<int,int> xy = scaleLatpLon(i.y(), i.x());
		if (pts.empty() || xy != pts.back()) pts.push_back(xy);
	}
#endif
}

// Scaling with naive self-intersection check - if we've added the new point
// within the last 5 points, then backtrack to the last time we added it
void TileBbox::scaleRing(Ring &points, Ring const &src) const {
	points.clear();
	points.reserve(src.size());
#ifdef __SSE2__
	const __m128d hiMinuend = _mm_set_pd(maxLatp, 0), loSubtrahend = _mm_set_sd(minLon), scale = _mm_set_pd(yscale, xscale);
	for(auto const &i: src) {
		__m128d sp;
		if (!scaleLatpLon2(_mm_loadu_pd(reinterpret_cast<const double*>(&i)), hiMinuend, loSubtrahend, scale, sp)) {
			auto scaled = scaleLatpLon(i.y(), i.x());
			sp = _mm_set_pd(scaled.second, scaled.first);
		}
		const size_t n = points.size();
		const double *d = reinterpret_cast<const double*>(points.data());
		size_t j = 1;
		for (; j < 5 && n >= 1 + j; j++)
			if (_mm_movemask_pd(_mm_cmpeq_pd(_mm_loadu_pd(d + 2 * (n - j)), sp)) == 3) break;
		if (j < 5 && n >= 1 + j) points.resize(n - j + 1);
		else {
			Point p;
			_mm_storeu_pd(reinterpret_cast<double*>(&p), sp);
			points.push_back(p);
		}
	}
#else
	for(auto const &i: src) {
		auto scaled = scaleLatpLon(i.y(), i.x()); // -> .first is x, .second is y
		bool found = false;
		for (size_t j=1; j<5; j++) {
			if (points.size() < 1+j) break;
			Point check = points[points.size()-j];
			if (check.x()==scaled.first && check.y()==scaled.second) {
				points.resize(points.size()-j+1); found=true; break;
			}
		}
		if (!found) points.push_back(Point(scaled.first,scaled.second));
	}
#endif
}

// Scaling that only drops points repeating their immediate predecessor. Used as
// a fallback for rings that scaleRing() collapses below 4 points: its
// backtracking window is positional, so a duplicated FIRST vertex shifts every
// later vertex by one, the closing vertex then matches that duplicate at j==4,
// and resize() truncates the whole ring. Without the backtracking there is
// nothing to mis-align, and the ring survives with its shape intact.
void TileBbox::scaleRingNoBacktrack(Ring &points, Ring const &src) const {
	points.clear();
	points.reserve(src.size());
	for(auto const &i: src) {
		auto scaled = scaleLatpLon(i.y(), i.x());
		if (!points.empty() &&
		    points.back().x()==scaled.first && points.back().y()==scaled.second)
			continue;
		points.push_back(Point(scaled.first,scaled.second));
	}
}

Ring TileBbox::scaleRing(Ring const &src) const {
	Ring points;
	scaleRing(points, src);
	return points;
}

void TileBbox::scaleGeometry(MultiPolygon &dst, MultiPolygon const &src) const {
	if (dst.size() < src.size())
		dst.resize(src.size());

	size_t polygonCount = 0;
	for(auto const &poly: src) {
		Polygon &p = dst[polygonCount];

		// Copy the outer ring
		scaleRing(p.outer(), poly.outer());
		if (p.outer().size()<4) {
			// A collapsed outer ring means the whole feature disappears from the
			// tile, at any size - so before giving up, retry without the
			// backtracking that can truncate a ring to two points. Every ring that
			// already survives is left untouched.
			scaleRingNoBacktrack(p.outer(), poly.outer());
			if (p.outer().size()<4)
				continue;
		}

		// Copy the inner rings
		if (p.inners().size() < poly.inners().size())
			p.inners().resize(poly.inners().size());
		size_t innerCount = 0;
		for(auto const &r: poly.inners()) {
			Ring &points = p.inners()[innerCount];
			scaleRing(points, r);
			if (points.size()>=4)
				innerCount++;
		}
		p.inners().resize(innerCount);
		polygonCount++;
	}
	dst.resize(polygonCount);
}

MultiPolygon TileBbox::scaleGeometry(MultiPolygon const &src) const {
	MultiPolygon dst;
	scaleGeometry(dst, src);
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
		double p2y = std::min(MaxLatp, std::max(MinLatp, p2.y()));
		double tileXf2 = lon2tilexf(p2.x(), baseZoom), tileYf2 = latp2tileyf(p2y, baseZoom);
		TileCoordinate tileX2 = static_cast<TileCoordinate>(tileXf2), tileY2 = static_cast<TileCoordinate>(tileYf2);
		tileSet.insert(TileCoordinates(tileX2, tileY2));
		if (it == points.begin()) continue;	// first point, so no line

		// Calculate p1 tile
		double p1y = std::min(MaxLatp, std::max(MinLatp, p1.y()));
		double tileXf1 = lon2tilexf(p1.x(), baseZoom), tileYf1 = latp2tileyf(p1y, baseZoom);
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

// Vector variant: same tiles as above, without hashing. Each vertex tile is computed once
// (the previous vertex tile is carried over), and lon2tilexf/latp2tileyf are inlined:
// multiplying by the exact power of two 2^z gives the same bits as scalbn(v, z).
static inline void pushTile(std::vector<TileCoordinates> &tiles, TileCoordinate x, TileCoordinate y) {
	if (tiles.empty() || tiles.back().x != x || tiles.back().y != y)
		tiles.push_back(TileCoordinates(x, y));
}

template<typename T>
void impl_insertIntermediateTilesVec(T const &points, uint baseZoom, std::vector<TileCoordinates> &tiles) {
	const size_t start = tiles.size();
	const unsigned zoom = (uint8_t)baseZoom;
	const double scale = zoom < 32 ? double(1u << zoom) : scalbn(1.0, (int)zoom);	// exactly 2^zoom
	TileCoordinate tileX1 = 0, tileY1 = 0;
	for (auto it = points.begin(); it != points.end(); ++it) {
		// Calculate p2 tile (same arithmetic as lon2tilexf/latp2tileyf)
		double p2y = std::min(MaxLatp, std::max(MinLatp, it->y()));
		double tileXf2 = ((it->x()+180.0) * (1/360.0)) * scale, tileYf2 = ((180.0-p2y) * (1/360.0)) * scale;
		TileCoordinate tileX2 = static_cast<TileCoordinate>(tileXf2), tileY2 = static_cast<TileCoordinate>(tileYf2);
		if (it == points.begin()) {	// first point, so no line
			pushTile(tiles, tileX2, tileY2);
			tileX1 = tileX2; tileY1 = tileY2;
			continue;
		}
		// Most points are in the tile of the previous one, which is the last tile pushed: nothing to add
		if (tileX2 == tileX1 && tileY2 == tileY1) continue;

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
			errorprev = error = dx;  // start in the middle of the square
			for (i=0 ; i < dx ; i++) {  // do not use the first point (already done)
				x += xstep;
				error += ddy;
				if (error > ddx){  // increment y if AFTER the middle ( > )
					y += ystep;
					error -= ddx;
					if (error + errorprev < ddx)  // bottom square also
						pushTile(tiles, x, y-ystep);
					else if (error + errorprev > ddx)  // left square also
						pushTile(tiles, x-xstep, y);
					else {  // corner: bottom and left squares also
						pushTile(tiles, x, y-ystep);
						pushTile(tiles, x-xstep, y);
					}
				}
				pushTile(tiles, x, y);
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
						pushTile(tiles, x-xstep, y);
					else if (error + errorprev > ddy)
						pushTile(tiles, x, y-ystep);
					else{
						pushTile(tiles, x-xstep, y);
						pushTile(tiles, x, y-ystep);
					}
				}
				pushTile(tiles, x, y);
				errorprev = error;
			}
		}
		pushTile(tiles, tileX2, tileY2);
		tileX1 = tileX2; tileY1 = tileY2;
	}
	sortUniqueTiles(tiles, start);
}

void insertIntermediateTiles(Linestring const &points, uint baseZoom, std::vector<TileCoordinates> &tiles)
{
	impl_insertIntermediateTilesVec(points, baseZoom, tiles);
}

void insertIntermediateTiles(Ring const &points, uint baseZoom, std::vector<TileCoordinates> &tiles)
{
	impl_insertIntermediateTilesVec(points, baseZoom, tiles);
}
