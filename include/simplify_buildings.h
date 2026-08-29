/*! \file */ 
#ifndef _SIMPLIFY_BUILDINGS_H
#define _SIMPLIFY_BUILDINGS_H

struct BuildingSimplifyConfig {
	// "Short" segment: the short side of the stub being removed (in coordinate units).
	// For lat/lon coordinates use e.g. 5.0/100000.0; scale accordingly for projected coords.
	double distance_filter = 5.0 / 100000.0;

	// Area guard: don't remove stubs where d1*d2 (product of the two stub sides) exceeds this.
	// Prevents collapsing significant right-angle jogs.  Set very large to disable.
	double area_filter = 50.0;

	// Narrow-edge exception: bypass the area guard when either stub side is shorter than this.
	double area_narrow = 2.0;

	// Minimum sine of the angle between the two outer lines (ring[km1]→ring[k] and
	// ring[kp2]→ring[kp3]).  When these lines are nearly parallel their intersection is at
	// infinity; the fallback midpoint passes the length check via the triangle inequality
	// and destroys a significant corner.  0.1 ≈ sin(5.7°) — reject anything more parallel.
	static constexpr double PARALLEL_TOL = 0.1;

	// Degrees tolerance for removing collinear points (straight-line removal).
	static constexpr double COLLINEAR_TOL = 8.0;

	// Degrees tolerance for snapping the intersection to a right angle.
	static constexpr double SNAP_TOL = 3.0;

	// Parametric tolerance for the proper-intersection test: values in [tol, 1-tol] count.
	// Keeps endpoint-touching segments from being treated as intersecting.
	static constexpr double INTERSECT_TOL = 0.001;
};

void simplify_building(Polygon &poly, const BuildingSimplifyConfig &cfg);
void simplify_building(MultiPolygon &mp, const BuildingSimplifyConfig &cfg);
void simplifyBuildings(MultiPolygon &mp, double max_distance);

#endif //_SIMPLIFY_BUILDINGS_H
