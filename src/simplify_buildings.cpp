#include "geom.h"
#include "simplify_buildings.h"
#include <vector>
#include <array>
#include <cmath>
#include <algorithm>
#include <iostream>

// Building simplification algorithm
// This aims to retain the typical rectilinear shape of a building while reducing vertices. Based on original Ruby code.

// Rtree storing (bounding_box, segment_endpoints) for self-intersection detection.
// We identify segments by their endpoint coordinates so we can skip segments that
// are being replaced during a candidate check, without needing index stability.
typedef std::pair<Point, Point> seg_pts;
typedef std::pair<Box, seg_pts> rtree_val;
typedef boost::geometry::index::rtree<rtree_val, boost::geometry::index::quadratic<16>> seg_rtree;


// ============================================================
// Low-level geometry helpers
// ============================================================

static double seg_len2(const Point &a, const Point &b) {
	double dx = b.x()-a.x(), dy = b.y()-a.y();
	return dx*dx + dy*dy;
}

static double seg_len(const Point &a, const Point &b) {
	return std::sqrt(seg_len2(a, b));
}

// Squared sine/cosine of the angle tolerances, so that angles can be tested
// with dot and cross products rather than trigonometry
static const double COLLINEAR_COS2 = std::pow(std::cos(BuildingSimplifyConfig::COLLINEAR_TOL * M_PI / 180.0), 2);
static const double SNAP_SIN2 = std::pow(std::sin(BuildingSimplifyConfig::SNAP_TOL * M_PI / 180.0), 2);
static const double PARALLEL_TOL2 = BuildingSimplifyConfig::PARALLEL_TOL * BuildingSimplifyConfig::PARALLEL_TOL;

// Rings with at most this many segments are checked for self-intersection by
// testing every segment, rather than by building an rtree
static const size_t BRUTE_FORCE_SEGMENTS = 64;

static Box make_box(const Point &a, const Point &b) {
	Box box;
	boost::geometry::envelope(Segment(a, b), box);
	return box;
}

static void rtree_add(seg_rtree &rt, const Point &a, const Point &b) {
	rt.insert({make_box(a, b), {a, b}});
}

// Convert a closed Boost ring to an open std::vector<point>
static std::vector<Point> ring_to_open(const Ring &r) {
	std::vector<Point> v(r.begin(), r.end());
	if (v.size() > 1 &&
	    v.front().x() == v.back().x() && v.front().y() == v.back().y())
		v.pop_back();
	return v;
}

// Write an open vector back into a Boost ring (re-closing it)
static void open_to_ring(const std::vector<Point> &v, Ring &r) {
	r.clear();
	for (auto const &p : v) r.push_back(p);
	if (!v.empty()) r.push_back(v[0]);
}

// Add all segments of an open ring to an rtree
static void add_ring_to_rtree(const std::vector<Point> &v, seg_rtree &rt) {
	size_t n = v.size();
	for (size_t k = 0; k < n; k++)
		rtree_add(rt, v[k], v[(k+1)%n]);
}

// Proper-intersection test (interior crossing only)
static bool properly_intersects(const Point &l1, const Point &l2,
                                const Point &m1, const Point &m2,
                                double tol = 0.001) {
	double a=l1.x(), b=l1.y(), c=l2.x(), d=l2.y();
	double p=m1.x(), q=m1.y(), r=m2.x(), s=m2.y();
	double det = (c-a)*(s-q) - (r-p)*(d-b);
	if (det == 0.0) return false;
	double lv = ((s-q)*(r-a) + (p-r)*(s-b)) / det;
	double gv = ((b-d)*(r-a) + (c-a)*(s-b)) / det;
	return (tol < lv && lv < 1.0-tol) && (tol < gv && gv < 1.0-tol);
}

// Whether segment p1→p2 is one of the four in `skip` (identified by exact endpoint
// coordinates — these are the segments being replaced in the current candidate operation)
static bool is_skipped(const Point &p1, const Point &p2, const std::array<seg_pts,4> &skip) {
	for (auto const &sk : skip) {
		if ((p1.x()==sk.first.x()  && p1.y()==sk.first.y() &&
		     p2.x()==sk.second.x() && p2.y()==sk.second.y()) ||
		    (p1.x()==sk.second.x() && p1.y()==sk.second.y() &&
		     p2.x()==sk.first.x()  && p2.y()==sk.first.y()))
			return true;
	}
	return false;
}

// Query the rtree for segments that properly intersect (a→b), other than those in `skip`
static bool hits_rtree(const Point &a, const Point &b,
                        const seg_rtree &rt,
                        const std::array<seg_pts,4> &skip,
                        double tol) {
	Box bbox = make_box(a, b);
	for (auto const &v : rt | boost::geometry::index::adaptors::queried(
	                               boost::geometry::index::intersects(bbox))) {
		const Point &p1 = v.second.first;
		const Point &p2 = v.second.second;
		if (!is_skipped(p1, p2, skip) && properly_intersects(a, b, p1, p2, tol)) return true;
	}
	return false;
}

// As hits_rtree, but testing each segment of the (open) ring in turn
static bool hits_ring(const Point &a, const Point &b,
                      const std::vector<Point> &ring,
                      const std::array<seg_pts,4> &skip,
                      double tol) {
	const double minX = std::min(a.x(), b.x()), maxX = std::max(a.x(), b.x());
	const double minY = std::min(a.y(), b.y()), maxY = std::max(a.y(), b.y());
	const size_t m = ring.size();
	for (size_t k = 0; k < m; k++) {
		const Point &p1 = ring[k];
		const Point &p2 = ring[k+1 == m ? 0 : k+1];
		if (std::max(p1.x(), p2.x()) < minX || std::min(p1.x(), p2.x()) > maxX ||
		    std::max(p1.y(), p2.y()) < minY || std::min(p1.y(), p2.y()) > maxY) continue;
		if (!is_skipped(p1, p2, skip) && properly_intersects(a, b, p1, p2, tol)) return true;
	}
	return false;
}

// Check against a foreign rtree (other rings already simplified — no skip needed).
static bool hits_foreign(const Point &a, const Point &b,
                         const seg_rtree &rt, double tol) {
	Box bbox = make_box(a, b);
	for (auto const &v : rt | boost::geometry::index::adaptors::queried(
	                               boost::geometry::index::intersects(bbox))) {
		if (properly_intersects(a, b, v.second.first, v.second.second, tol)) return true;
	}
	return false;
}

// ============================================================
// Step 1: Remove collinear points
// ============================================================

// Signed turn angle at 'turn' in degrees, normalised to [0, 360).
static double turn_angle(const Point &from, const Point &turn, const Point &to) {
	double rad = std::atan2(to.y()-turn.y(), to.x()-turn.x())
	           - std::atan2(from.y()-turn.y(), from.x()-turn.x());
	double deg = rad * (180.0 / M_PI);
	return deg - 360.0 * std::floor(deg / 360.0);
}

// Whether the turn angle at 'turn' is within COLLINEAR_TOL° of 180°: that is, the
// directions to its neighbours are at least 180°-COLLINEAR_TOL apart
static bool is_collinear(const Point &from, const Point &turn, const Point &to) {
	double ux = from.x()-turn.x(), uy = from.y()-turn.y();
	double vx = to.x()-turn.x(),   vy = to.y()-turn.y();
	double uu = ux*ux + uy*uy, vv = vx*vx + vy*vy;
	if (uu == 0.0 || vv == 0.0) {
		// A repeated point has no direction: take the angle as atan2 defines it
		double a = turn_angle(from, turn, to);
		return a >= 180.0-BuildingSimplifyConfig::COLLINEAR_TOL && a <= 180.0+BuildingSimplifyConfig::COLLINEAR_TOL;
	}
	double dot = ux*vx + uy*vy;
	return dot < 0.0 && dot*dot >= COLLINEAR_COS2 * uu * vv;
}

// Remove points whose turn angle is within tol° of 180° (collinear with neighbours),
// earliest first. The first and last points are kept.
static void remove_collinear(std::vector<Point> &ring) {
	size_t n = ring.size();
	if (n < 3) return;
	size_t kept = 1;	// ring[0..kept) survive so far
	for (size_t j = 1; j < n; j++) {
		// ring[j] is the next point: a removal changes the previous kept point's
		// neighbours, so it may now be collinear too
		while (kept >= 2 && is_collinear(ring[kept-2], ring[kept-1], ring[j])) kept--;
		ring[kept++] = ring[j];
	}
	ring.resize(kept);
}

// ============================================================
// Step 2: Find intersection of two extended lines
// ============================================================

// Returns the intersection point of the line through pL1→pL2 and the line through pM1→pM2.
// When the angle between them is within snap_tol° of 90°, snaps to an exact right angle
// by projecting pM2 perpendicularly onto line L.
// Falls back to midpoint of the two "inner" points if lines are parallel.
static Point line_intersection(const Point &pL1, const Point &pL2,
                               const Point &pM1, const Point &pM2) {
	double xL1=pL1.x(), yL1=pL1.y(), xL2=pL2.x(), yL2=pL2.y();
	double xM1=pM1.x(), yM1=pM1.y(), xM2=pM2.x(), yM2=pM2.y();

	// Express lines as ax+by+c=0
	double a1=yL1-yL2, b1=xL2-xL1, c1=(yL2-yL1)*xL1-(xL2-xL1)*yL1;
	double a2=yM1-yM2, b2=xM2-xM1, c2=(yM2-yM1)*xM1-(xM2-xM1)*yM1;

	// Snap to 90°: drop perpendicular from pM2 onto line L. The lines are within
	// SNAP_TOL° of a right angle when the cosine of the angle between them is
	// within sin(SNAP_TOL) of zero.
	double dot = a1*a2 + b1*b2, cross = a2*b1 - a1*b2;
	if (dot*dot < SNAP_SIN2 * (dot*dot + cross*cross)) {
		double denom = (xL2-xL1)*(xL2-xL1) + (yL2-yL1)*(yL2-yL1);
		if (denom < 1e-20) return Point((xL2+xM1)/2.0, (yL2+yM1)/2.0);
		double t = ((xM2-xL1)*(xL2-xL1) + (yM2-yL1)*(yL2-yL1)) / denom;
		return Point(xL1+(xL2-xL1)*t, yL1+(yL2-yL1)*t);
	}

	// General case: find intersection
	double num = a1*b2-a2*b1;
	if (std::abs(num) < 1e-20) return Point((xL2+xM1)/2.0, (yL2+yM1)/2.0);
	return Point((b1*c2-b2*c1)/num, (c1*a2-c2*a1)/num);
}

// Simplify a single open ring (ring[0] != ring.back()) in-place.
// `other_rt` is an optional rtree of already-simplified rings to prevent
// cross-ring self-intersections (pass nullptr if not needed).
static void simplify_open_ring(std::vector<Point> &ring,
                               const BuildingSimplifyConfig &cfg,
                               const seg_rtree* other_rt = nullptr) {
	remove_collinear(ring);

	const double shortest2 = cfg.distance_filter * cfg.distance_filter;
	const double area2 = cfg.area_filter * cfg.area_filter;
	const double narrow2 = cfg.area_narrow * cfg.area_narrow;
	std::vector<double> segs2;
	std::vector<size_t> candidates;
	std::vector<Point> new_ring;

	while (true) {
		size_t m = ring.size();
		if (m < 4) break;

		// segs2[k] = |ring[k] → ring[(k+1)%m]|²
		segs2.resize(m);
		candidates.clear();
		for (size_t k = 0; k < m; k++) {
			segs2[k] = seg_len2(ring[k], ring[(k+1)%m]);
			if (segs2[k] < shortest2) candidates.push_back(k);
		}
		if (candidates.empty()) break;

		// The shortest stub that passes every check is removed (the earliest, if
		// several are equally short), so try them shortest first
		std::sort(candidates.begin(), candidates.end(), [&](size_t a, size_t b) {
			return segs2[a] != segs2[b] ? segs2[a] < segs2[b] : a < b;
		});

		// Long rings are checked for self-intersection with an rtree, built when first needed
		const bool use_rtree = m > BRUTE_FORCE_SEGMENTS;
		seg_rtree rt;
		bool rt_built = false;
		auto hits_self = [&](const Point &a, const Point &b, const std::array<seg_pts,4> &skip) {
			if (!use_rtree) return hits_ring(a, b, ring, skip, BuildingSimplifyConfig::INTERSECT_TOL);
			if (!rt_built) {
				std::vector<rtree_val> values;
				values.reserve(m);
				for (size_t k = 0; k < m; k++)
					values.push_back({make_box(ring[k], ring[(k+1)%m]), {ring[k], ring[(k+1)%m]}});
				rt = seg_rtree(values.begin(), values.end());
				rt_built = true;
			}
			return hits_rtree(a, b, rt, skip, BuildingSimplifyConfig::INTERSECT_TOL);
		};

		int    best     = -1;
		Point  best_xy;

		for (size_t k : candidates) {
			double d1 = segs2[k];
			double d2 = segs2[(k+1)%m];

			// Area filter: skip if the stub area d1*d2 is too large,
			// unless one side is a narrow sliver (area_narrow exception).
			if (d1*d2 > area2 && d1 >= narrow2 && d2 >= narrow2)
				continue;

			// Indices of the six points surrounding the stub.
			// The stub to remove: ring[k] → ring[kp1] → ring[kp2]
			// (ring[k] is replaced; ring[kp1] and ring[kp2] are deleted)
			size_t km1=(k+m-1)%m;
			size_t kp1=(k+1)%m, kp2=(k+2)%m, kp3=(k+3)%m;

			// Reject if the two outer lines are nearly parallel.
			{
				double dx_L = ring[k].x()-ring[km1].x(), dy_L = ring[k].y()-ring[km1].y();
				double dx_M = ring[kp3].x()-ring[kp2].x(), dy_M = ring[kp3].y()-ring[kp2].y();
				double cross = dx_L*dy_M - dy_L*dx_M;
				if (cross*cross < PARALLEL_TOL2 * (dx_L*dx_L+dy_L*dy_L) * (dx_M*dx_M+dy_M*dy_M)) continue;
			}

			// Intersection of line(ring[km1],ring[k]) and line(ring[kp2],ring[kp3])
			Point xy = line_intersection(ring[km1], ring[k],
			                             ring[kp2], ring[kp3]);

			// Reject if the new path (km1→xy→kp3) is longer than the old detour
			double old_len = std::sqrt(segs2[km1]) + std::sqrt(segs2[k]) + std::sqrt(segs2[kp1]) + std::sqrt(segs2[kp2]);
			double new_len = seg_len(xy, ring[km1]) + seg_len(xy, ring[kp3]);
			if (new_len > old_len) continue;

			// Self-intersection check.
			// The four segments being replaced are skipped; any other intersection rejects.
			std::array<seg_pts,4> skip = {{
				{ring[km1], ring[k]},    // segs[km1]
				{ring[k],   ring[kp1]}, // segs[k]
				{ring[kp1], ring[kp2]}, // segs[kp1]
				{ring[kp2], ring[kp3]}, // segs[kp2]
			}};
			if (hits_self(ring[km1], xy,        skip)) continue;
			if (hits_self(xy,        ring[kp3], skip)) continue;

			// Cross-ring check (e.g. simplified inners when processing the outer)
			if (other_rt) {
				if (hits_foreign(ring[km1], xy,        *other_rt, BuildingSimplifyConfig::INTERSECT_TOL)) continue;
				if (hits_foreign(xy,        ring[kp3], *other_rt, BuildingSimplifyConfig::INTERSECT_TOL)) continue;
			}

			best     = (int)k;
			best_xy  = xy;
			break;
		}

		if (best < 0) break;

		// Apply: delete ring[kp1] and ring[kp2], set ring[k] = best_xy.
		// Build a new ring to avoid index-shifting bugs on wraparound cases.
		size_t k   = (size_t)best;
		size_t kp1 = (k+1)%m, kp2 = (k+2)%m;

		new_ring.clear();
		for (size_t i = 0; i < m; i++) {
			if (i == kp1 || i == kp2) continue;
			new_ring.push_back(i == k ? best_xy : ring[i]);
		}
		ring.swap(new_ring);
	}
}

// ============================================================
// Public API
// ============================================================

// Simplify a single polygon (outer ring + inner rings) in-place.
void simplify_building(Polygon &poly, const BuildingSimplifyConfig &cfg) {
	seg_rtree inners_rt;

	for (auto &inner : poly.inners()) {
		std::vector<Point> open = ring_to_open(inner);
		simplify_open_ring(open, cfg, inners_rt.empty() ? nullptr : &inners_rt);
		open_to_ring(open, inner);
		add_ring_to_rtree(open, inners_rt);
	}

	std::vector<Point> open = ring_to_open(poly.outer());
	simplify_open_ring(open, cfg, inners_rt.empty() ? nullptr : &inners_rt);
	open_to_ring(open, poly.outer());
}

// Simplify all polygons in a multipolygon.
void simplify_building(MultiPolygon &mp, const BuildingSimplifyConfig &cfg) {
	for (auto &poly : mp) simplify_building(poly, cfg);
}

// Simplify with config derived from standard max_distance
void simplifyBuildings(MultiPolygon &mp, double max_distance) {
	BuildingSimplifyConfig cfg;
	cfg.distance_filter = max_distance;
	cfg.area_filter = max_distance * max_distance / 2;
	cfg.area_narrow = max_distance / 2;
	simplify_building(mp, cfg);
}
