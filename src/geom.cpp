#define BOOST_GEOMETRY_NO_ROBUSTNESS
#include "geom.h"

#include <boost/geometry/geometries/segment.hpp>
#include <boost/geometry/index/rtree.hpp>
#include <boost/geometry/algorithms/buffer.hpp>
#include <boost/geometry/strategies/buffer.hpp>

#include "geometry/correct.hpp"
#include <limits>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/irange.hpp>

typedef boost::geometry::model::segment<Point> simplify_segment;
typedef boost::geometry::index::rtree<simplify_segment, boost::geometry::index::quadratic<16>> simplify_rtree;

template<typename GeometryType>
static inline void simplify_ring(GeometryType const &input, GeometryType &output, double distance, simplify_rtree const &outer_rtree = simplify_rtree())
{
    std::deque<std::size_t> nodes(input.size());
    for(std::size_t i = 0; i < input.size(); ++i)
        nodes[i] = i;

	simplify_rtree rtree(
		boost::irange<std::size_t>(0, input.size() - 1)
		| boost::adaptors::transformed(std::function<simplify_segment(std::size_t)>([&input](std::size_t i) {
			return simplify_segment(input[i], input[i+1]);
		})));

    Box envelope; boost::geometry::envelope(input, envelope);

	for(std::size_t pq = input.size() - 2; pq--; ) {
        auto entry = pq;

        auto start = nodes[entry];
        auto middle = nodes[entry + 1];
        auto end = nodes[entry + 2];

        if (input[middle].x()==envelope.min_corner().x() ||
            input[middle].y()==envelope.min_corner().y() ||
            input[middle].x()==envelope.max_corner().x() ||
            input[middle].y()==envelope.max_corner().y()) continue;

        simplify_segment line(input[start], input[end]);

        double max_comp_distance = 0.0;
		std::size_t max_comp_i = start + 1;

        for(auto i = start + 1; i < end; ++i) {
			auto comp_distance = boost::geometry::comparable_distance(line, input[i]);
            if(comp_distance > max_comp_distance) {
				max_comp_distance = comp_distance;
				max_comp_i = i;
			}
		}

        if(boost::geometry::distance(line, input[max_comp_i]) < distance) {
			std::size_t query_count = 0;
            for(auto const &result: rtree | boost::geometry::index::adaptors::queried(boost::geometry::index::intersects(line)))
				++query_count;
            for(auto const &result: outer_rtree | boost::geometry::index::adaptors::queried(boost::geometry::index::intersects(line)))
				++query_count;

			std::size_t expected_count = std::min<std::size_t>(4, nodes.size() - 1);
            if(query_count == expected_count) {
                nodes.erase(nodes.begin() + entry + 1);
                rtree.remove(simplify_segment(input[start], input[middle]));
                rtree.remove(simplify_segment(input[middle], input[end]));
                rtree.insert(line);
            }
        }
    }

	output.resize(nodes.size());
	for(std::size_t i = 0; i < nodes.size(); ++i)
		output[i] = input[nodes[i]];
}

Polygon simplify(Polygon const &p, double max_distance) 
{
	Polygon result;

	simplify_rtree outer_rtree(
		boost::irange<std::size_t>(0, p.outer().size() - 1)
		| boost::adaptors::transformed(std::function<simplify_segment(std::size_t)>([&p](std::size_t i) {
			return simplify_segment(p.outer()[i], p.outer()[i+1]);
		})));

	for(auto const &inner: p.inners()) {
		Ring new_inner;
		simplify_ring(inner, new_inner, max_distance, outer_rtree);

		std::reverse(new_inner.begin(), new_inner.end());
		if(new_inner.size() > 3 && boost::geometry::perimeter(new_inner) > 3 * max_distance) {
			simplify_combine(result.inners(), std::move(new_inner));
		}
	}

	simplify_rtree inners_rtree;
	for(auto &inner: result.inners()) {
		std::reverse(inner.begin(), inner.end());

		inners_rtree.insert(
			boost::irange<std::size_t>(0, inner.size() - 1)
			| boost::adaptors::transformed(std::function<simplify_segment(std::size_t)>([&inner](std::size_t i) {
				return simplify_segment(inner[i], inner[i+1]);
			})));
	}

	simplify_ring(p.outer(), result.outer(), max_distance, inners_rtree);
	if(result.outer().size() > 3 && boost::geometry::perimeter(result.outer()) > 3 * max_distance) {
		return result;
	}

	result = Polygon();
	return result;
}

Linestring simplify(Linestring const &ls, double max_distance) 
{
	Linestring result;
	boost::geometry::simplify(ls, result, max_distance);
	return result;
}

MultiPolygon simplify(MultiPolygon const &mp, double max_distance) 
{
	MultiPolygon result_mp;
	for(auto const &p: mp) {
		Polygon new_p = simplify(p, max_distance);
		if(!new_p.outer().empty()) {
			geom::correct(new_p);
			simplify_combine(result_mp, std::move(new_p));
		}
	}

	return result_mp;
}

void make_valid(MultiPolygon &mp)
{
	MultiPolygon result;
	for(auto const &p: mp) {
		geometry::correct(p, result, 1E-12);
	}
	mp = result;
}

// ---------------
// Conservative validity precheck (see geom.h).
// Every decision is taken only when it is clearly away from the degenerate
// case. An orientation determinant counts as non-zero only if it exceeds
// (a) a relative bound on its own rounding error, (b) the point-to-line
// distance tolerance `tol` (relative to the coordinate magnitude, far above
// Boost's epsilon point equality) and (c) Boost's side_by_triangle zero band
// (epsilon * max(1, |coordinate differences|), whichever point it permutes
// first) with a wide margin. Anything borderline answers "unsure".

namespace {

const double kPrecheckEps = 1e-9;
const std::size_t kPrecheckMaxHoles = 256;
const std::size_t kPrecheckRetained = 1 << 16;

struct PrecheckSeg {
	double minX, maxX, minY, maxY;
	const Point *a; // segment a[0] -> a[1]
	uint32_t ring, idx, count; // ring number, segment index, segments in the ring
};
struct PrecheckBox {
	double minX, maxX, minY, maxY;
};
struct PrecheckScratch {
	std::vector<PrecheckSeg> segs;
	std::vector<PrecheckBox> boxes;
	void trim() {
		if (segs.capacity() > kPrecheckRetained) std::vector<PrecheckSeg>().swap(segs);
		if (boxes.capacity() > kPrecheckRetained) std::vector<PrecheckBox>().swap(boxes);
	}
};
thread_local PrecheckScratch precheckScratch;

inline double max3(double a, double b, double c) { return std::max(a, std::max(b, c)); }

// Orientation of c relative to a->b: +1 counter-clockwise, -1 clockwise, 0 when too close to call.
inline int clearOrient(const Point &a, const Point &b, const Point &c, double tol) {
	const double dx = b.x() - a.x(), dy = b.y() - a.y();
	const double px = c.x() - a.x(), py = c.y() - a.y();
	const double qx = c.x() - b.x(), qy = c.y() - b.y();
	const double t1 = dx * py, t2 = dy * px;
	const double d = t1 - t2;
	const double f = std::max(max3(1.0, std::fabs(dx), std::fabs(dy)),
	                          std::max(max3(std::fabs(px), std::fabs(py), std::fabs(qx)), std::fabs(qy)));
	const double bound = max3(kPrecheckEps * (std::fabs(t1) + std::fabs(t2)),
	                          tol * (std::fabs(dx) + std::fabs(dy)),
	                          64 * std::numeric_limits<double>::epsilon() * f * f);
	if (std::fabs(d) > bound) return d > 0 ? 1 : -1;
	return 0;
}

// Coordinates finite; accumulates the largest magnitude.
inline bool ringFinite(const Ring &r, double &maxAbs) {
	for (const Point &p : r) {
		const double x = p.x(), y = p.y();
		if (!std::isfinite(x) || !std::isfinite(y)) return false;
		maxAbs = max3(maxAbs, std::fabs(x), std::fabs(y));
	}
	return true;
}

// Single-ring tests: closed, >= 4 points, consecutive points either exactly
// equal (Boost allows duplicate points; such zero-length segments are skipped)
// or clearly distinct, area clearly non-zero with the orientation Boost
// expects, no spikes. Appends the ring's segments and returns its bbox.
bool ringClear(const Ring &r, bool interior, double tol, uint32_t ringIdx, std::vector<PrecheckSeg> &segs, PrecheckBox &box) {
	const std::size_t n = r.size();
	if (n < 4 || n >= std::numeric_limits<uint32_t>::max()) return false;
	const std::size_t m = n - 1; // segments: i runs r[i] -> r[i+1]
	if (r[0].x() != r[m].x() || r[0].y() != r[m].y()) return false;

	const std::size_t first = segs.size();
	box.minX = box.minY = std::numeric_limits<double>::infinity();
	box.maxX = box.maxY = -std::numeric_limits<double>::infinity();
	for (std::size_t i = 0; i < m; i++) {
		const Point &a = r[i], &b = r[i + 1];
		if (a.x() == b.x() && a.y() == b.y()) continue;
		if (std::fabs(b.x() - a.x()) <= tol && std::fabs(b.y() - a.y()) <= tol) return false;
		PrecheckSeg s;
		s.minX = std::min(a.x(), b.x()); s.maxX = std::max(a.x(), b.x());
		s.minY = std::min(a.y(), b.y()); s.maxY = std::max(a.y(), b.y());
		s.a = &a;
		s.ring = ringIdx; s.idx = static_cast<uint32_t>(segs.size() - first);
		segs.push_back(s);
		box.minX = std::min(box.minX, s.minX); box.maxX = std::max(box.maxX, s.maxX);
		box.minY = std::min(box.minY, s.minY); box.maxY = std::max(box.maxY, s.maxY);
	}
	const std::size_t count = segs.size() - first;
	if (count < 3) return false;
	for (std::size_t k = first; k < segs.size(); k++) segs[k].count = static_cast<uint32_t>(count);

	// Signed area (doubled) relative to r[0], positive = counter-clockwise. It must
	// clearly exceed both our rounding error and that of Boost's (x1+x2)*(y1-y2) sum.
	const double x0 = r[0].x(), y0 = r[0].y();
	double area = 0, areaScale = 0, boostScale = 0;
	for (std::size_t i = 1; i + 1 < m; i++) {
		const double t1 = (r[i].x() - x0) * (r[i + 1].y() - y0);
		const double t2 = (r[i].y() - y0) * (r[i + 1].x() - x0);
		area += t1 - t2;
		areaScale += std::fabs(t1) + std::fabs(t2);
	}
	for (std::size_t i = 0; i < m; i++)
		boostScale += (std::fabs(r[i].x()) + std::fabs(r[i + 1].x())) * std::fabs(r[i].y() - r[i + 1].y());
	const double relEps = std::max(kPrecheckEps, 8.0 * (n + 4) * std::numeric_limits<double>::epsilon());
	if (!(std::fabs(area) > relEps * areaScale && std::fabs(area) > relEps * boostScale)) return false;
	const bool clockwise = boost::geometry::point_order<Polygon>::value == boost::geometry::clockwise;
	const bool wantPositive = clockwise == interior; // clockwise polygon: CW outer, CCW inners
	if (wantPositive ? area <= 0 : area >= 0) return false;

	// Consecutive segments a->b, b->c only share b unless they fold back (spike).
	for (std::size_t k = 0; k < count; k++) {
		const PrecheckSeg &prev = segs[first + (k == 0 ? count - 1 : k - 1)];
		const Point &a = prev.a[0];
		const Point &b = segs[first + k].a[0];
		const Point &c = segs[first + k].a[1];
		if (clearOrient(a, b, c, tol) != 0) continue;
		// (nearly) collinear: must clearly continue forward
		const double t1 = (b.x() - a.x()) * (c.x() - b.x());
		const double t2 = (b.y() - a.y()) * (c.y() - b.y());
		if (!(t1 + t2 > kPrecheckEps * (std::fabs(t1) + std::fabs(t2)))) return false;
	}
	return true;
}

inline bool boxesApart(const PrecheckBox &a, const PrecheckBox &b, double tol) {
	return a.maxX + tol < b.minX || b.maxX + tol < a.minX || a.maxY + tol < b.minY || b.maxY + tol < a.minY;
}

// Two segments are clearly disjoint (adjacent segments of a ring are handled by the spike test).
inline bool segsClear(const PrecheckSeg &s, const PrecheckSeg &t, double tol) {
	if (s.ring == t.ring) {
		const uint32_t lo = std::min(s.idx, t.idx), hi = std::max(s.idx, t.idx);
		if (hi - lo == 1 || (lo == 0 && hi == s.count - 1)) return true;
	}
	if (s.maxX + tol < t.minX || t.maxX + tol < s.minX || s.maxY + tol < t.minY || t.maxY + tol < s.minY)
		return true;
	const Point &p1 = s.a[0], &p2 = s.a[1], &q1 = t.a[0], &q2 = t.a[1];
	const int o1 = clearOrient(p1, p2, q1, tol), o2 = clearOrient(p1, p2, q2, tol);
	if (o1 != 0 && o1 == o2) return true; // q entirely on one side of line p
	const int o3 = clearOrient(q1, q2, p1, tol), o4 = clearOrient(q1, q2, p2, tol);
	return o3 != 0 && o3 == o4; // p entirely on one side of line q; else crossing, touching or unsure
}

// All pairs of segments clearly disjoint: sweep over segments sorted by minX.
// The answer does not depend on the order (every x-overlapping pair is tested).
bool segmentsClear(std::vector<PrecheckSeg> &segs, double tol) {
	const std::size_t n = segs.size();
	if (n <= 32) {
		for (std::size_t i = 0; i < n; i++)
			for (std::size_t j = i + 1; j < n; j++)
				if (!segsClear(segs[i], segs[j], tol)) return false;
		return true;
	}
	std::sort(segs.begin(), segs.end(), [](const PrecheckSeg &a, const PrecheckSeg &b) { return a.minX < b.minX; });
	for (std::size_t i = 0; i < n; i++) {
		const double limit = segs[i].maxX + tol;
		for (std::size_t j = i + 1; j < n && segs[j].minX <= limit; j++)
			if (!segsClear(segs[i], segs[j], tol)) return false;
	}
	return true;
}

// Crossing-number test: 1 = h clearly inside r, -1 = clearly outside, 0 = unsure.
int pointClearlyInRing(const Point &h, const Ring &r, double tol) {
	bool inside = false;
	const double hx = h.x(), hy = h.y();
	for (std::size_t i = 0; i + 1 < r.size(); i++) {
		const Point &a = r[i], &b = r[i + 1];
		if ((a.y() > hy + tol && b.y() > hy + tol) || (a.y() < hy - tol && b.y() < hy - tol)) continue;
		if (std::fabs(a.y() - hy) <= tol || std::fabs(b.y() - hy) <= tol) return 0;
		// a and b are clearly on opposite sides of the horizontal line through h
		if (std::max(a.x(), b.x()) < hx - tol) continue;
		if (std::min(a.x(), b.x()) > hx + tol) { inside = !inside; continue; }
		const int o = clearOrient(a, b, h, tol);
		if (o == 0) return 0;
		if ((b.y() > a.y()) == (o > 0)) inside = !inside; // the ray to +x crosses a->b
	}
	return inside ? 1 : -1;
}

// Location of a ring that is known not to touch r: tries a few of its vertices.
int ringClearlyInRing(const Ring &inner, const Ring &r, double tol) {
	const std::size_t m = inner.size() - 1;
	const std::size_t tries = std::min<std::size_t>(m, 4);
	for (std::size_t k = 0; k < tries; k++) {
		const int loc = pointClearlyInRing(inner[k * m / tries], r, tol);
		if (loc != 0) return loc;
	}
	return 0;
}

bool polygonClear(const Polygon &p, PrecheckScratch &scratch, PrecheckBox &outerBox) {
	const Ring &outer = p.outer();
	const auto &inners = p.inners();
	if (inners.size() > kPrecheckMaxHoles) return false;
	double maxAbs = 0;
	if (!ringFinite(outer, maxAbs)) return false;
	for (const auto &inner : inners)
		if (!ringFinite(inner, maxAbs)) return false;
	const double tol = kPrecheckEps * std::max(1.0, maxAbs);

	std::vector<PrecheckSeg> &segs = scratch.segs;
	segs.clear();
	PrecheckBox innerBoxes[kPrecheckMaxHoles];
	if (!ringClear(outer, false, tol, 0, segs, outerBox)) return false;
	for (std::size_t i = 0; i < inners.size(); i++)
		if (!ringClear(inners[i], true, tol, static_cast<uint32_t>(i + 1), segs, innerBoxes[i])) return false;

	// No two rings touch, and no ring touches itself.
	if (!segmentsClear(segs, tol)) return false;

	// Holes lie inside the outer ring and are not nested in each other.
	for (std::size_t i = 0; i < inners.size(); i++) {
		if (boxesApart(innerBoxes[i], outerBox, tol)) return false;
		if (ringClearlyInRing(inners[i], outer, tol) != 1) return false;
	}
	for (std::size_t i = 0; i < inners.size(); i++)
		for (std::size_t j = i + 1; j < inners.size(); j++) {
			if (boxesApart(innerBoxes[i], innerBoxes[j], tol)) continue;
			if (ringClearlyInRing(inners[i], inners[j], tol) != -1 ||
			    ringClearlyInRing(inners[j], inners[i], tol) != -1) return false;
		}
	return true;
}

bool multiPolygonClear(const MultiPolygon &mp, PrecheckScratch &scratch) {
	if (mp.empty()) return false;
	PrecheckBox box;
	if (mp.size() == 1) return polygonClear(mp[0], scratch, box);

	// Polygons must have clearly disjoint bboxes (outer rings; holes lie inside them).
	std::vector<PrecheckBox> &boxes = scratch.boxes;
	boxes.clear();
	double maxAbs = 0;
	for (const auto &p : mp) {
		if (p.outer().empty()) return false;
		box.minX = box.minY = std::numeric_limits<double>::infinity();
		box.maxX = box.maxY = -std::numeric_limits<double>::infinity();
		for (const Point &pt : p.outer()) {
			const double x = pt.x(), y = pt.y();
			if (!std::isfinite(x) || !std::isfinite(y)) return false;
			box.minX = std::min(box.minX, x); box.maxX = std::max(box.maxX, x);
			box.minY = std::min(box.minY, y); box.maxY = std::max(box.maxY, y);
		}
		maxAbs = std::max(maxAbs, max3(std::fabs(box.minX), std::fabs(box.maxX), std::max(std::fabs(box.minY), std::fabs(box.maxY))));
		boxes.push_back(box);
	}
	const double tol = kPrecheckEps * std::max(1.0, maxAbs);
	std::sort(boxes.begin(), boxes.end(), [](const PrecheckBox &a, const PrecheckBox &b) { return a.minX < b.minX; });
	for (std::size_t i = 0; i < boxes.size(); i++) {
		const double limit = boxes[i].maxX + tol;
		for (std::size_t j = i + 1; j < boxes.size() && boxes[j].minX <= limit; j++)
			if (!boxesApart(boxes[i], boxes[j], tol)) return false;
	}

	for (const auto &p : mp)
		if (!polygonClear(p, scratch, box)) return false;
	return true;
}

} // namespace

bool polygonDefinitelyValid(const Polygon &p) {
	PrecheckBox box;
	const bool ok = polygonClear(p, precheckScratch, box);
	precheckScratch.trim();
	return ok;
}

bool multiPolygonDefinitelyValid(const MultiPolygon &mp) {
	const bool ok = multiPolygonClear(mp, precheckScratch);
	precheckScratch.trim();
	return ok;
}


// Repair a single (possibly invalid) polygon in an area-preserving way and
// append the resulting valid polygon(s) to `out`. Returns true on success.
// A repair is only accepted if the resulting area stays within
// [minArea, maxArea]:
//  - the LOWER bound catches a collapse (dissolve/buffer dropping most of the
//    covered area on huge inputs);
//  - the UPPER bound catches a FILLED HOLE, which is what used to make an ocean
//    polygon swallow a whole island. Measured on real data, the separation is
//    large: genuine repairs of quantised ocean polygons change the area by
//    <=0.01 %, whereas filling one island hole grew it by 32 %.
static bool repair_one_polygon(const Polygon &p, double minArea, double maxArea, MultiPolygon &out)
{
	auto acceptable = [&](const MultiPolygon &candidate) {
		if (!geom::is_valid(candidate)) return false;
		const double a = std::abs(geom::area(candidate));
		return a >= minArea && a <= maxArea;
	};

	// 0) Fast path: a ring without holes whose only fault is its orientation
	// (e.g. after the y-flip into tile coordinates). On such a simple ring the
	// dissolve finds no intersections and just reverses it, so do that directly.
	if (p.inners().empty()) {
		Polygon rev = p;
		std::reverse(rev.outer().begin(), rev.outer().end());
		const double revArea = std::abs(geom::area(rev));
		bool revValid = false;
		if (revArea > 1E-12 && revArea >= minArea && revArea <= maxArea) {
			const bool pre = polygonDefinitelyValid(rev);
			revValid = pre || geom::is_valid(rev);
		}
		if (revValid) {
			out.push_back(std::move(rev));
			return true;
		}
	}

	// 1) Dissolve (resolves self-intersections of this single polygon).
	try {
		MultiPolygon fixed;
		geometry::correct(p, fixed, 1E-12);
		if (acceptable(fixed)) {
			for (auto &fp : fixed) out.push_back(std::move(fp));
			return true;
		}
	} catch (const std::exception &) {
		// fall through to the buffer attempt
	}

	// 2) Zero-width buffer as a last resort.
	try {
		MultiPolygon buffered;
		geom::strategy::buffer::distance_symmetric<double> distanceStrategy(0.0);
		geom::strategy::buffer::side_straight sideStrategy;
		geom::strategy::buffer::join_miter joinStrategy;
		geom::strategy::buffer::end_flat endStrategy;
		geom::strategy::buffer::point_square pointStrategy;

		geom::buffer(p, buffered, distanceStrategy, sideStrategy, joinStrategy, endStrategy, pointStrategy);
		geom::correct(buffered);
		if (acceptable(buffered)) {
			for (auto &bp : buffered) out.push_back(std::move(bp));
			return true;
		}
	} catch (const std::exception &) {
		// keep best-effort polygon
	}

	return false;
}

bool repair_multi_polygon(MultiPolygon &mp, bool knownInvalid, AreaBound areaBound)
{
	if (!knownInvalid) {
		const bool pre = multiPolygonDefinitelyValid(mp);
		if (pre || geom::is_valid(mp)) return true;
	}

	// Repair PER POLYGON, area-preserving. Running make_valid/buffer on the whole
	// multipolygon can catastrophically COLLAPSE large/complex inputs: a clipped
	// reservoir with >1000 rings dropped ~99% of its area, leaving missing lake
	// tiles at low zoom. Conversely, simply keeping the whole invalid geometry
	// lets a single self-intersecting ring render as a spurious "spike".
	//
	// Fixing each polygon independently gets the best of both: a self-touching
	// ring is cleaned (no spike) while the rest stays intact, and we avoid the
	// O(n^2) cross-polygon union that caused the collapse. A polygon whose repair
	// would not preserve its area is kept as-is (invalid but complete renders;
	// only a tiny local artefact, never a dropped area).
	MultiPolygon out;
	bool allValid = true;
	for (const auto &p : mp) {
		// With a single polygon, the whole-multipolygon check above already
		// established it is invalid.
		if (mp.size() > 1) {
			const bool pre = polygonDefinitelyValid(p);
			if (pre || geom::is_valid(p)) {
				out.push_back(p);
				continue;
			}
		}
		// Lenient lower bound: resolving a self-intersection legitimately changes
		// a single polygon's (shoelace) area, so anything down to half the
		// original is accepted. Per-polygon repair cannot trigger the
		// cross-polygon union that previously caused the catastrophic ~99%
		// collapse, so this only rejects a genuine local collapse.
		// The upper bound is only applied with AreaBound::Strict, i.e. for geometry
		// that was not simplified. Applying it to simplified geometry would be
		// wrong: it rejects legitimate repairs there and measurably made things
		// worse (5 -> 20 invalid features in one tile).
		const double origArea = std::abs(geom::area(p));
		const double maxArea = areaBound == AreaBound::Strict
		                     ? 1.01 * origArea
		                     : std::numeric_limits<double>::infinity();
		if (!repair_one_polygon(p, 0.5 * origArea, maxArea, out)) {
			out.push_back(p);
			allValid = false;
		}
	}

	mp = std::move(out);
	return allValid;
}

// ---------------
// Union multipolygons
// Groups polygons into connected components by bbox intersection (via R-tree +
// union-find), then runs the binary reduction only within each component.
// Disjoint polygons are concatenated directly, skipping the expensive union_()
// call that Boost still runs even for non-overlapping geometry.
void union_many(std::vector<MultiPolygon> &to_unify) {
	if (to_unify.size() < 2) return;

	namespace bgi = boost::geometry::index;
	typedef std::pair<Box, size_t> BoxIdx;

	std::vector<Box> boxes(to_unify.size());
	for (size_t i = 0; i < to_unify.size(); i++)
		boost::geometry::envelope(to_unify[i], boxes[i]);

	// Union-find with path compression
	std::vector<size_t> parent(to_unify.size());
	for (size_t i = 0; i < to_unify.size(); i++) parent[i] = i;
	std::function<size_t(size_t)> find = [&](size_t x) -> size_t {
		return parent[x] == x ? x : (parent[x] = find(parent[x]));
	};

	// Incrementally build an R-tree; unite each polygon with all prior ones
	// whose bboxes intersect (transitivity is handled by union-find)
	bgi::rtree<BoxIdx, bgi::quadratic<16>> rtree;
	for (size_t i = 0; i < to_unify.size(); i++) {
		if (boost::geometry::is_empty(to_unify[i])) continue;
		for (auto const &v : rtree | bgi::adaptors::queried(bgi::intersects(boxes[i]))) {
			size_t ri = find(i), rj = find(v.second);
			if (ri != rj) parent[ri] = rj;
		}
		rtree.insert({boxes[i], i});
	}

	// Bucket indices by component root (roots are in 0..n-1)
	std::vector<std::vector<size_t>> components(to_unify.size());
	for (size_t i = 0; i < to_unify.size(); i++)
		components[find(i)].push_back(i);

	// Union within each component; concatenate all results into to_unify[0]
	MultiPolygon result;
	for (auto &comp : components) {
		if (comp.empty()) continue;
		if (comp.size() == 1) {
			for (auto &p : to_unify[comp[0]]) result.push_back(std::move(p));
			continue;
		}
		std::vector<MultiPolygon> sub;
		sub.reserve(comp.size());
		for (size_t idx : comp) sub.push_back(std::move(to_unify[idx]));

		size_t step = 1, half_step;
		do {
			half_step = step; step *= 2;
			for (size_t i = 0; i + half_step < sub.size(); i += step) {
				MultiPolygon unified;
				boost::geometry::union_(sub[i], sub[i + half_step], unified);
				sub[i] = std::move(unified);
			}
		} while (step < sub.size());

		for (auto &p : sub[0]) result.push_back(std::move(p));
	}
	to_unify[0] = std::move(result);
}

// ---------------
// Sutherland-Hodgeman clipper
// ported from Volodymyr Agafonkin's https://github.com/mapbox/lineclip

// intersect a segment against one of the 4 lines that make up the bbox
Point intersect_edge(Point const &a, Point const &b, char edge, Box const &bbox) {
	if (edge & 8) return Point(a.x() + (b.x() - a.x()) * (bbox.max_corner().y() - a.y()) / (b.y() - a.y()), bbox.max_corner().y()); // top
	if (edge & 4) return Point(a.x() + (b.x() - a.x()) * (bbox.min_corner().y() - a.y()) / (b.y() - a.y()), bbox.min_corner().y()); // bottom
	if (edge & 2) return Point(bbox.max_corner().x(), a.y() + (b.y() - a.y()) * (bbox.max_corner().x() - a.x()) / (b.x() - a.x())); // right
	if (edge & 1) return Point(bbox.min_corner().x(), a.y() + (b.y() - a.y()) * (bbox.min_corner().x() - a.x()) / (b.x() - a.x())); // left
	throw std::runtime_error("intersect called on non-intersection");
}

// bit code reflects the point position relative to the bbox:
//         left  mid  right
//    top  1001  1000  1010
//    mid  0001  0000  0010
// bottom  0101  0100  0110

char bit_code(Point const &p, Box const &bbox) {
	char code = 0;
	if      (p.x() < bbox.min_corner().x()) code |= 1; // left
	else if (p.x() > bbox.max_corner().x()) code |= 2; // right
	if      (p.y() < bbox.min_corner().y()) code |= 4; // bottom
	else if (p.y() > bbox.max_corner().y()) code |= 8; // top
	return code;
}

// Sutherland-Hodgeman polygon clipping algorithm
// Returns true if any clipping was applied (false: points left untouched).
bool fast_clip(Ring &points, Box const &bbox, Ring &result) {
	bool modified = false;
	// clip against each side of the clip rectangle
	for (char edge = 1; edge <= 8; edge *= 2) {
		bool needsClip = false;
		for (auto const &p: points) {
			if (bit_code(p, bbox) & edge) {
				needsClip = true;
				break;
			}
		}
		if (!needsClip) continue;

		modified = true;
		result.clear();
		result.reserve(points.size() + 4);
		Point prev = points[points.size() - 1];
		bool prevInside = (bit_code(prev, bbox) & edge)==0;

		for (unsigned int i = 0; i<points.size(); i++) {
			Point p = points[i];
			bool inside = (bit_code(p, bbox) & edge)==0;

			// if segment goes through the clip window, add an intersection
			if (inside!=prevInside) result.emplace_back(intersect_edge(prev, p, edge, bbox));
			if (inside) result.emplace_back(p); // add a point if it's inside
			prev = p;
			prevInside = inside;
		}
		points.swap(result);
		if (points.size()==0) break;
	}
	return modified;
}

bool fast_clip(Ring &points, Box const &bbox) {
	Ring result;
	return fast_clip(points, bbox, result);
}

// Wrappers for polygon/multipolygon
bool fast_clip(Polygon &polygon, Box const &bbox) {
	Ring result;
	bool modified = fast_clip(polygon.outer(), bbox, result);
	if (polygon.outer().empty()) {
		modified = modified || !polygon.inners().empty();
		polygon.inners().resize(0);
		return modified;
	}
	for (auto &inner: polygon.inners()) {
		if (fast_clip(inner, bbox, result)) modified = true;
	}
	size_t innersBefore = polygon.inners().size();
	polygon.inners().erase(std::remove_if(
		polygon.inners().begin(), polygon.inners().end(), 
		[](const Ring &inner) -> bool { return inner.empty(); }),
		polygon.inners().end());
	return modified || polygon.inners().size() != innersBefore;
}

bool fast_clip(MultiPolygon &mp, Box const &bbox) {
	bool modified = false;
	for (auto &polygon: mp) {
		if (fast_clip(polygon, bbox)) modified = true;
	}
	size_t sizeBefore = mp.size();
	mp.erase(std::remove_if(
		mp.begin(), mp.end(), 
		[](const Polygon &poly) -> bool { return poly.outer().empty(); }),
		mp.end());
	return modified || mp.size() != sizeBefore;
}
