#define BOOST_GEOMETRY_NO_ROBUSTNESS
#include "geom.h"

#include <boost/geometry/geometries/segment.hpp>
#include <boost/geometry/index/rtree.hpp>
#include <boost/geometry/algorithms/buffer.hpp>
#include <boost/geometry/strategies/buffer.hpp>

#include "geometry/correct.hpp"

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
const std::size_t kPrecheckMaxWork = 1 << 26; // ring vertices walked by point-in-ring tests
const std::size_t kPrecheckRetained = 1 << 16;

struct PrecheckSeg {
	double minX, maxX, minY, maxY;
	const Point *a; // segment a[0] -> a[1]
	uint32_t ring, idx, count; // ring number, segment index, segments in the ring
};
struct PrecheckBox {
	double minX, maxX, minY, maxY;
	uint32_t id; // ring or polygon number
};
struct PrecheckScratch {
	std::vector<PrecheckSeg> segs;
	std::vector<PrecheckBox> boxes;
	std::vector<std::pair<uint32_t, uint32_t>> pairs;
	std::vector<uint32_t> bandStart, bandFill, bandSegs, crossed;
	void trim() {
		if (segs.capacity() > kPrecheckRetained) std::vector<PrecheckSeg>().swap(segs);
		if (boxes.capacity() > kPrecheckRetained) std::vector<PrecheckBox>().swap(boxes);
		if (pairs.capacity() > kPrecheckRetained) std::vector<std::pair<uint32_t, uint32_t>>().swap(pairs);
		if (bandStart.capacity() > kPrecheckRetained) std::vector<uint32_t>().swap(bandStart);
		if (bandFill.capacity() > kPrecheckRetained) std::vector<uint32_t>().swap(bandFill);
		if (bandSegs.capacity() > kPrecheckRetained) std::vector<uint32_t>().swap(bandSegs);
		if (crossed.capacity() > kPrecheckRetained) std::vector<uint32_t>().swap(crossed);
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

// Calls f(idA, idB) for every pair of boxes that are not clearly apart (sweep over
// boxes sorted by minX); returns false as soon as f does.
template <class F>
bool forOverlappingBoxes(std::vector<PrecheckBox> &boxes, double tol, F f) {
	std::sort(boxes.begin(), boxes.end(), [](const PrecheckBox &a, const PrecheckBox &b) { return a.minX < b.minX; });
	for (std::size_t i = 0; i < boxes.size(); i++) {
		const double limit = boxes[i].maxX + tol;
		for (std::size_t j = i + 1; j < boxes.size() && boxes[j].minX <= limit; j++)
			if (!boxesApart(boxes[i], boxes[j], tol) && !f(boxes[i].id, boxes[j].id)) return false;
	}
	return true;
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

// Whether the ray from h to +x crosses segment a->b: 1 = yes, -1 = no, 0 = unsure.
// Vertices are classed as above (y > hy) or not with exact comparisons (the
// half-open rule), so a ray through a vertex is counted consistently and the
// crossing parity is exact; only the side of h is decided with a tolerance.
inline int rayClearlyCrosses(const Point &h, const Point &a, const Point &b, double tol) {
	const double hx = h.x(), hy = h.y();
	if ((a.y() > hy) == (b.y() > hy)) return -1;
	if (std::max(a.x(), b.x()) < hx) return -1;
	if (std::min(a.x(), b.x()) > hx) return 1;
	const int o = clearOrient(a, b, h, tol);
	if (o == 0) return 0;
	return (b.y() > a.y()) == (o > 0) ? 1 : -1;
}

// Crossing-number test: 1 = h clearly inside r, -1 = clearly outside, 0 = unsure.
int pointClearlyInRing(const Point &h, const Ring &r, double tol) {
	bool inside = false;
	for (std::size_t i = 0; i + 1 < r.size(); i++) {
		const int c = rayClearlyCrosses(h, r[i], r[i + 1], tol);
		if (c == 0) return 0;
		if (c > 0) inside = !inside;
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

// Holes lie inside the outer ring (ring 0) and outside each other. `segs` holds the
// segments of all rings of p, known not to touch. They are bucketed into horizontal
// bands, so that a ray from a vertex of each hole is tested against the segments of
// that vertex's band only, counting crossings per ring (odd = inside that ring).
bool holesClearlyPlaced(const Polygon &p, PrecheckScratch &scratch, double tol) {
	const auto &inners = p.inners();
	const std::vector<PrecheckSeg> &segs = scratch.segs;
	if (inners.empty()) return true;
	if (segs.size() >= std::numeric_limits<uint32_t>::max()) return false;

	double minY = std::numeric_limits<double>::infinity(), maxY = -minY;
	for (const PrecheckSeg &s : segs) { minY = std::min(minY, s.minY); maxY = std::max(maxY, s.maxY); }
	minY -= tol; maxY += tol;
	// About 4 segments per band; fewer bands while tall segments would make the index much bigger than segs
	std::size_t bands = std::max<std::size_t>(1, segs.size() / 4);
	double scale = 0;
	auto bandOf = [&](double y) {
		const double b = (y - minY) * scale;
		return b <= 0 ? std::size_t(0) : std::min(bands - 1, static_cast<std::size_t>(b));
	};
	std::size_t entries;
	for (;;) {
		scale = bands / (maxY - minY);
		entries = 0;
		for (const PrecheckSeg &s : segs) entries += bandOf(s.maxY + tol) - bandOf(s.minY - tol) + 1;
		if (entries <= 4 * segs.size() || bands == 1) break;
		bands = (bands + 1) / 2;
	}
	if (entries >= std::numeric_limits<uint32_t>::max()) return false;

	// Segments of band b: bandSegs[bandStart[b] .. bandStart[b+1])
	std::vector<uint32_t> &start = scratch.bandStart, &fill = scratch.bandFill, &items = scratch.bandSegs;
	start.assign(bands + 1, 0);
	for (const PrecheckSeg &s : segs)
		for (std::size_t b = bandOf(s.minY - tol), hi = bandOf(s.maxY + tol); b <= hi; b++) start[b + 1]++;
	for (std::size_t b = 0; b < bands; b++) start[b + 1] += start[b];
	fill.assign(start.begin(), start.end() - 1);
	items.resize(entries);
	for (std::size_t k = 0; k < segs.size(); k++)
		for (std::size_t b = bandOf(segs[k].minY - tol), hi = bandOf(segs[k].maxY + tol); b <= hi; b++)
			items[fill[b]++] = static_cast<uint32_t>(k);

	// 1 = h inside the outer ring and no other hole, -1 = not, 0 = unsure
	std::vector<uint32_t> &crossed = scratch.crossed;
	auto locate = [&](const Point &h, uint32_t ownRing) {
		crossed.clear();
		const std::size_t b = bandOf(h.y());
		for (uint32_t k = start[b]; k < start[b + 1]; k++) {
			const PrecheckSeg &s = segs[items[k]];
			if (s.ring == ownRing) continue;
			const int c = rayClearlyCrosses(h, s.a[0], s.a[1], tol);
			if (c == 0) return 0;
			if (c > 0) crossed.push_back(s.ring);
		}
		std::sort(crossed.begin(), crossed.end());
		bool inOuter = false;
		for (std::size_t i = 0; i < crossed.size(); ) {
			std::size_t j = i;
			while (j < crossed.size() && crossed[j] == crossed[i]) j++;
			if ((j - i) % 2 == 1) {
				if (crossed[i] != 0) return -1; // inside another hole
				inOuter = true;
			}
			i = j;
		}
		return inOuter ? 1 : -1;
	};

	// A ring known not to touch the others lies wherever any of its vertices does
	for (std::size_t i = 0; i < inners.size(); i++) {
		const Ring &hole = inners[i];
		const std::size_t m = hole.size() - 1;
		const std::size_t tries = std::min<std::size_t>(m, 8);
		int loc = 0;
		for (std::size_t k = 0; k < tries && loc == 0; k++)
			loc = locate(hole[k * m / tries], static_cast<uint32_t>(i + 1));
		if (loc != 1) return false;
	}
	return true;
}

bool polygonClear(const Polygon &p, PrecheckScratch &scratch, PrecheckBox &outerBox) {
	const Ring &outer = p.outer();
	const auto &inners = p.inners();
	if (inners.size() >= std::numeric_limits<uint32_t>::max()) return false;
	double maxAbs = 0;
	if (!ringFinite(outer, maxAbs)) return false;
	for (const auto &inner : inners)
		if (!ringFinite(inner, maxAbs)) return false;
	const double tol = kPrecheckEps * std::max(1.0, maxAbs);

	std::vector<PrecheckSeg> &segs = scratch.segs;
	segs.clear();
	if (!ringClear(outer, false, tol, 0, segs, outerBox)) return false;
	PrecheckBox holeBox;
	for (std::size_t i = 0; i < inners.size(); i++)
		if (!ringClear(inners[i], true, tol, static_cast<uint32_t>(i + 1), segs, holeBox)) return false;

	// No two rings touch, and no ring touches itself.
	if (!segmentsClear(segs, tol)) return false;

	// Holes lie inside the outer ring and are not nested in each other.
	return holesClearlyPlaced(p, scratch, tol);
}

// a's outer ring, known not to touch any ring of b, lies outside b's interior:
// outside b's outer ring, or inside one of b's holes.
bool polygonClearlyOutside(const Polygon &a, const Polygon &b, double tol) {
	const int loc = ringClearlyInRing(a.outer(), b.outer(), tol);
	if (loc != 1) return loc == -1;
	for (const auto &hole : b.inners())
		if (ringClearlyInRing(a.outer(), hole, tol) == 1) return true;
	return false;
}

bool multiPolygonClear(const MultiPolygon &mp, PrecheckScratch &scratch) {
	if (mp.empty()) return false;
	PrecheckBox box;
	if (mp.size() == 1) return polygonClear(mp[0], scratch, box);
	if (mp.size() >= std::numeric_limits<uint32_t>::max()) return false;

	// Polygons whose bboxes (outer rings; holes lie inside them) overlap get the pairwise tests below.
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
		box.id = static_cast<uint32_t>(boxes.size());
		boxes.push_back(box);
	}
	const double tol = kPrecheckEps * std::max(1.0, maxAbs);
	std::vector<std::pair<uint32_t, uint32_t>> &pairs = scratch.pairs;
	pairs.clear();
	std::size_t work = 0;
	if (!forOverlappingBoxes(boxes, tol, [&](uint32_t a, uint32_t b) {
		pairs.emplace_back(a, b);
		work += mp[a].outer().size() + mp[b].outer().size();
		for (const auto &hole : mp[a].inners()) work += hole.size();
		for (const auto &hole : mp[b].inners()) work += hole.size();
		return work <= kPrecheckMaxWork;
	})) return false;

	for (const auto &p : mp)
		if (!polygonClear(p, scratch, box)) return false;
	if (pairs.empty()) return true;

	// No ring of one polygon touches a ring of another. (This re-tests the rings
	// within each polygon too, at the multipolygon's possibly larger tolerance.)
	std::vector<PrecheckSeg> &segs = scratch.segs;
	segs.clear();
	uint32_t ringIdx = 0;
	for (const auto &p : mp) {
		if (!ringClear(p.outer(), false, tol, ringIdx++, segs, box)) return false;
		for (const auto &inner : p.inners())
			if (!ringClear(inner, true, tol, ringIdx++, segs, box)) return false;
	}
	if (!segmentsClear(segs, tol)) return false;

	// Interiors are disjoint: neither polygon of an overlapping pair lies inside the other.
	for (const auto &pr : pairs)
		if (!polygonClearlyOutside(mp[pr.first], mp[pr.second], tol) ||
		    !polygonClearlyOutside(mp[pr.second], mp[pr.first], tol)) return false;
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
// `minArea` is the lower bound on the repaired area we are willing to accept.
static bool repair_one_polygon(const Polygon &p, double minArea, MultiPolygon &out)
{
	// 0) Fast path: a ring without holes whose only fault is its orientation
	// (e.g. after the y-flip into tile coordinates). On such a simple ring the
	// dissolve finds no intersections and just reverses it, so do that directly.
	if (p.inners().empty()) {
		Polygon rev = p;
		std::reverse(rev.outer().begin(), rev.outer().end());
		const double revArea = std::abs(geom::area(rev));
		bool revValid = false;
		if (revArea > 1E-12 && revArea >= minArea) {
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
		if (geom::is_valid(fixed) && std::abs(geom::area(fixed)) >= minArea) {
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
		if (geom::is_valid(buffered) && std::abs(geom::area(buffered)) >= minArea) {
			for (auto &bp : buffered) out.push_back(std::move(bp));
			return true;
		}
	} catch (const std::exception &) {
		// keep best-effort polygon
	}

	return false;
}

bool repair_multi_polygon(MultiPolygon &mp, bool knownInvalid, bool *wholeValid)
{
	if (wholeValid) *wholeValid = false;
	if (!knownInvalid) {
		const bool pre = multiPolygonDefinitelyValid(mp);
		if (pre || geom::is_valid(mp)) {
			if (wholeValid) *wholeValid = true;
			return true;
		}
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
		// Lenient threshold: resolving a self-intersection legitimately changes a
		// single polygon's (shoelace) area, so anything down to half the original
		// is accepted. Per-polygon repair cannot trigger the cross-polygon union
		// that previously caused the catastrophic ~99% collapse, so this only
		// rejects a genuine local collapse.
		const double minArea = 0.5 * std::abs(geom::area(p));
		if (!repair_one_polygon(p, minArea, out)) {
			out.push_back(p);
			allValid = false;
		}
	}

	mp = std::move(out);
	// A single valid polygon is a valid multipolygon
	if (wholeValid) *wholeValid = allValid && mp.size() == 1;
	return allValid;
}

// ---------------
// Union multipolygons

// Union of two multipolygons whose own polygons don't overlap (each is an input
// polygon or an earlier union result). Only the polygons whose bbox intersects
// a bbox on the other side are passed to union_(); the rest can't overlap
// anything and are copied through. union_() tests every ring that has no
// intersections against the whole other geometry, so passing it two large
// multipolygons that only meet along a seam is quadratic.
static void union_near(MultiPolygon &a, MultiPolygon &b, MultiPolygon &out) {
	namespace bgi = boost::geometry::index;
	typedef std::pair<Box, size_t> BoxIdx;

	std::vector<BoxIdx> bBoxes(b.size());
	for (size_t j = 0; j < b.size(); j++) {
		boost::geometry::envelope(b[j], bBoxes[j].first);
		bBoxes[j].second = j;
	}
	bgi::rtree<BoxIdx, bgi::quadratic<16>> rtree(bBoxes);

	MultiPolygon aNear, bNear;
	std::vector<bool> bIsNear(b.size(), false);
	for (auto &p : a) {
		Box box;
		boost::geometry::envelope(p, box);
		bool isNear = false;
		for (auto const &v : rtree | bgi::adaptors::queried(bgi::intersects(box))) {
			bIsNear[v.second] = true;
			isNear = true;
		}
		(isNear ? aNear : out).push_back(std::move(p));
	}
	for (size_t j = 0; j < b.size(); j++)
		(bIsNear[j] ? bNear : out).push_back(std::move(b[j]));

	if (aNear.empty()) return;
	MultiPolygon unified;
	boost::geometry::union_(aNear, bNear, unified);
	for (auto &p : unified) out.push_back(std::move(p));
}

// Splits the multipolygons into their polygons, sorts those along a Z-order
// curve and runs a binary reduction with union_near(). Thanks to the sort, each
// step unions two neighbouring regions, so only polygons along their seam go
// through union_(), and polygons that touch nothing are never passed to it.
void union_many(std::vector<MultiPolygon> &to_unify) {
	if (to_unify.size() < 2) return;

	std::vector<Polygon> polys;
	for (auto &mp : to_unify)
		for (auto &p : mp)
			if (!p.outer().empty()) polys.push_back(std::move(p));
	if (polys.empty()) { to_unify[0].clear(); return; }

	// Z-order (Morton) key of each polygon's bbox centre within the overall bbox
	std::vector<Point> centres(polys.size());
	Box all;
	boost::geometry::assign_inverse(all);
	for (size_t i = 0; i < polys.size(); i++) {
		Box box;
		boost::geometry::envelope(polys[i], box);
		boost::geometry::centroid(box, centres[i]);
		boost::geometry::expand(all, box);
	}
	const double minX = all.min_corner().x(), minY = all.min_corner().y();
	const double w = std::max(all.max_corner().x() - minX, 1e-300), h = std::max(all.max_corner().y() - minY, 1e-300);
	auto spread = [](uint32_t v) -> uint32_t {
		v &= 0xFFFF;
		v = (v | (v << 8)) & 0x00FF00FF;
		v = (v | (v << 4)) & 0x0F0F0F0F;
		v = (v | (v << 2)) & 0x33333333;
		v = (v | (v << 1)) & 0x55555555;
		return v;
	};
	std::vector<std::pair<uint32_t, size_t>> order(polys.size());
	for (size_t i = 0; i < polys.size(); i++) {
		uint32_t x = static_cast<uint32_t>((centres[i].x() - minX) / w * 65535.0);
		uint32_t y = static_cast<uint32_t>((centres[i].y() - minY) / h * 65535.0);
		order[i] = { spread(x) | (spread(y) << 1), i };
	}
	std::sort(order.begin(), order.end());

	std::vector<MultiPolygon> sub(polys.size());
	for (size_t i = 0; i < order.size(); i++) sub[i].push_back(std::move(polys[order[i].second]));

	size_t step = 1, half_step;
	do {
		half_step = step; step *= 2;
		for (size_t i = 0; i + half_step < sub.size(); i += step) {
			MultiPolygon unified;
			union_near(sub[i], sub[i + half_step], unified);
			sub[i] = std::move(unified);
		}
	} while (step < sub.size());

	to_unify[0] = std::move(sub[0]);
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

// ---------------
// Exact clipping of a valid polygon to an axis-aligned box.
//
// Sutherland-Hodgman (fast_clip) is exact for convex input but leaves zero-width
// "bridges" along the box edge wherever a concave polygon leaves and re-enters
// the box; Boost reports those as self-intersections. This clipper instead cuts
// every ring into the chains that lie inside the box and reconnects them along
// the box boundary (Weiler-Atherton specialised to a rectangle), so a valid input
// gives a valid output. Whenever the input does not fit its assumptions (numerical
// trouble, a hole it cannot place) it gives up and returns false, and the caller
// falls back to Boost.

namespace {

struct RectClipper {
	double x0, y0, x1, y1, w, h, perimeter;

	explicit RectClipper(const Box &box) :
		x0(box.min_corner().x()), y0(box.min_corner().y()),
		x1(box.max_corner().x()), y1(box.max_corner().y()),
		w(x1 - x0), h(y1 - y0), perimeter(2 * (x1 - x0) + 2 * (y1 - y0)) {}

	bool inside(const Point &p) const {
		return p.x() >= x0 && p.x() <= x1 && p.y() >= y0 && p.y() <= y1;
	}
	bool strictlyInside(const Point &p) const {
		return p.x() > x0 && p.x() < x1 && p.y() > y0 && p.y() < y1;
	}
	// Segment a->b, both inside the box, runs through its interior (not along an edge)
	bool throughInterior(const Point &a, const Point &b) const {
		return strictlyInside(Point((a.x() + b.x()) / 2, (a.y() + b.y()) / 2));
	}
	// Segment b->c, b on the boundary, starts off into the interior
	bool entersInterior(const Point &b, const Point &c) const {
		double t0, t1;
		Point pa, pb;
		return clipSegment(b, c, t0, t1, pa, pb) && t0 == 0 && t1 > 0 && throughInterior(b, pb);
	}
	// A ring p->t->q touches the boundary at t (not a corner) from inside. True if the
	// polygon interior (right of the ring) covers the boundary on both sides of t,
	// i.e. the edge direction lies in the clockwise sweep from t->q to t->p.
	bool pinchAt(const Point &p, const Point &t, const Point &q) const {
		if ((t.x() == x0 || t.x() == x1) && (t.y() == y0 || t.y() == y1)) return false;
		const double ex = (t.y() == y0 || t.y() == y1) ? 1 : 0, ey = 1 - ex;
		const double qx = q.x() - t.x(), qy = q.y() - t.y();
		auto clockwiseFromQ = [&](double vx, double vy) {
			const double a = -std::atan2(qx * vy - qy * vx, qx * vx + qy * vy);
			return a < 0 ? a + 2 * M_PI : a;
		};
		return clockwiseFromQ(ex, ey) < clockwiseFromQ(p.x() - t.x(), p.y() - t.y());
	}

	// Position along the box boundary, clockwise from the top-left corner.
	// Returns false if p is not exactly on the boundary.
	bool boundaryPos(const Point &p, double &s) const {
		if (p.y() == y1) s = p.x() - x0;
		else if (p.x() == x1) s = w + (y1 - p.y());
		else if (p.y() == y0) s = w + h + (x1 - p.x());
		else if (p.x() == x0) s = 2 * w + h + (p.y() - y0);
		else return false;
		return true;
	}

	// Liang-Barsky: the part [t0,t1] of segment a->b inside the closed box, with
	// its end points snapped exactly onto the box edge they were clipped against.
	bool clipSegment(const Point &a, const Point &b, double &t0, double &t1, Point &pa, Point &pb) const {
		const double dx = b.x() - a.x(), dy = b.y() - a.y();
		const double p[4] = { -dx, dx, -dy, dy };
		const double q[4] = { a.x() - x0, x1 - a.x(), a.y() - y0, y1 - a.y() };
		int e0 = -1, e1 = -1;
		t0 = 0; t1 = 1;
		for (int k = 0; k < 4; k++) {
			if (p[k] == 0) {
				if (q[k] < 0) return false;
				continue;
			}
			const double r = q[k] / p[k];
			if (p[k] < 0) { if (r > t0) { t0 = r; e0 = k; } }
			else          { if (r < t1) { t1 = r; e1 = k; } }
		}
		if (t0 > t1) return false;
		pa = e0 < 0 ? a : onEdge(a, dx, dy, t0, e0);
		pb = e1 < 0 ? b : onEdge(a, dx, dy, t1, e1);
		return true;
	}

	Point onEdge(const Point &a, double dx, double dy, double t, int edge) const {
		switch (edge) {
			case 0:  return Point(x0, std::min(std::max(a.y() + t * dy, y0), y1));
			case 1:  return Point(x1, std::min(std::max(a.y() + t * dy, y0), y1));
			case 2:  return Point(std::min(std::max(a.x() + t * dx, x0), x1), y0);
			default: return Point(std::min(std::max(a.x() + t * dx, x0), x1), y1);
		}
	}

	// Corners in clockwise boundary order, starting top-left
	Point corner(int i) const {
		switch (i) {
			case 0:  return Point(x0, y1);
			case 1:  return Point(x1, y1);
			case 2:  return Point(x1, y0);
			default: return Point(x0, y0);
		}
	}
	double cornerPos(int i) const {
		switch (i) {
			case 0:  return 0;
			case 1:  return w;
			case 2:  return w + h;
			default: return 2 * w + h;
		}
	}
};

enum class RingClip { Inside, Outside, Cut, Fail };

struct RectChain {
	std::vector<Point> points;
	double entry, exit;
	bool used = false;
	bool touchExit = false; // ends at a vertex touching the boundary from inside; the next chain starts there
};

// Cut a closed ring into the chains inside the box. Inside: every vertex is
// inside (so is the ring). Outside: no part of the ring enters the box.
RingClip clipRing(const RectClipper &rc, const Ring &ring, std::vector<RectChain> &chains) {
	if (ring.size() < 4) return RingClip::Fail;
	const std::size_t n = ring.size() - 1; // closed: ring[n] == ring[0]
	std::size_t start = n;
	for (std::size_t i = 0; i < n; i++)
		if (!rc.inside(ring[i])) { start = i; break; }
	if (start == n) return RingClip::Inside;

	// Start at a vertex outside the box, so every chain begins with an entry.
	std::vector<Point> current;
	bool building = false, any = false;
	auto finish = [&]() -> bool {
		RectChain c;
		if (current.size() < 2 || !rc.boundaryPos(current.front(), c.entry) || !rc.boundaryPos(current.back(), c.exit)) return false;
		c.points.swap(current);
		chains.push_back(std::move(c));
		current.clear();
		building = false;
		any = true;
		return true;
	};
	for (std::size_t k = 0; k < n; k++) {
		const Point &a = ring[(start + k) % n];
		const Point &b = ring[(start + k + 1) % n];
		double t0, t1;
		Point pa, pb;
		const bool hit = rc.clipSegment(a, b, t0, t1, pa, pb) && t1 > t0;
		if (!hit || (building && t0 > 0)) {
			if (building && !finish()) return RingClip::Fail;
			if (!hit) continue;
		}
		if (!building) {
			current.push_back(pa);
			building = true;
		}
		if (!geom::equals(pb, current.back())) current.push_back(pb);
		if (t1 < 1) {
			if (!finish()) return RingClip::Fail;
		} else if (current.size() >= 2 && !rc.strictlyInside(b) && rc.throughInterior(current[current.size() - 2], b) &&
		           rc.entersInterior(b, ring[(start + k + 2) % n]) &&
		           rc.pinchAt(current[current.size() - 2], b, ring[(start + k + 2) % n])) {
			// b touches the boundary from inside (reached through the interior, left
			// into the interior) and the polygon lies along the boundary on both sides
			// of it. Kept in one chain, the boundary walk would run through b and make
			// the ring touch itself there: split the chain at b, so b is an exit and
			// an entry, with the polygon pinched in two there.
			if (!finish()) return RingClip::Fail;
			chains.back().touchExit = true;
			current.push_back(b);
			building = true;
		}
	}
	// The walk ends at the outside start vertex, so the last chain must have exited
	if (building) return RingClip::Fail;
	return any ? RingClip::Cut : RingClip::Outside;
}

// Crossing-number test: 1 inside, -1 outside, 0 on the boundary.
int pointInRing(const Point &p, const Ring &ring) {
	bool in = false;
	for (std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
		const Point &a = ring[i], &b = ring[j];
		if (geom::equals(a, p)) return 0;
		if ((a.y() > p.y()) != (b.y() > p.y())) {
			const double x = a.x() + (p.y() - a.y()) * (b.x() - a.x()) / (b.y() - a.y());
			if (x == p.x()) return 0;
			if (x > p.x()) in = !in;
		}
	}
	return in ? 1 : -1;
}

// Where a ring that does not cross `outer` lies relative to it (0: unsure).
int ringInRing(const Ring &ring, const Ring &outer) {
	for (const Point &p : ring) {
		const int r = pointInRing(p, outer);
		if (r != 0) return r;
	}
	return 0;
}

bool clipPolygon(const RectClipper &rc, const Polygon &poly, MultiPolygon &out) {
	std::vector<RectChain> chains;
	const RingClip outerClip = clipRing(rc, poly.outer(), chains);
	if (outerClip == RingClip::Fail) return false;
	if (outerClip == RingClip::Inside) {
		out.push_back(poly);
		return true;
	}

	std::vector<const Ring*> insideHoles, outsideHoles;
	for (const Ring &inner : poly.inners()) {
		switch (clipRing(rc, inner, chains)) {
			case RingClip::Fail:    return false;
			case RingClip::Inside:  insideHoles.push_back(&inner); break;
			case RingClip::Outside: outsideHoles.push_back(&inner); break;
			case RingClip::Cut:     break;
		}
	}

	const std::size_t firstOuter = out.size();
	if (chains.empty()) {
		// No ring crosses the box: it is either inside the polygon or disjoint from it
		const Point centre((rc.x0 + rc.x1) / 2, (rc.y0 + rc.y1) / 2);
		const int inOuter = pointInRing(centre, poly.outer());
		if (inOuter == 0) return false;
		bool inside = inOuter > 0;
		for (const Ring *hole : outsideHoles) {
			const int inHole = pointInRing(centre, *hole);
			if (inHole == 0) return false;
			if (inHole > 0) inside = false;
		}
		if (!inside) return insideHoles.empty();
		Polygon boxPoly;
		for (int i = 0; i < 4; i++) boxPoly.outer().push_back(rc.corner(i));
		boxPoly.outer().push_back(rc.corner(0));
		out.push_back(std::move(boxPoly));
	} else {
		// Polygon interiors are on the right of every ring (clockwise outers,
		// anticlockwise holes), so from each exit the boundary walk goes clockwise
		// to the next entry.
		std::vector<std::pair<double, std::size_t>> entries;
		entries.reserve(chains.size());
		for (std::size_t i = 0; i < chains.size(); i++) entries.emplace_back(chains[i].entry, i);
		std::sort(entries.begin(), entries.end());

		for (std::size_t first = 0; first < chains.size(); first++) {
			if (chains[first].used) continue;
			Ring ring;
			std::size_t c = first;
			while (true) {
				RectChain &chain = chains[c];
				chain.used = true;
				for (const Point &p : chain.points)
					if (ring.empty() || !geom::equals(p, ring.back())) ring.push_back(p);

				// From a touch exit, the entry at the same point is the far side of the
				// touch: walk on to the next one
				auto it = chain.touchExit
					? std::upper_bound(entries.begin(), entries.end(), std::make_pair(chain.exit, std::numeric_limits<std::size_t>::max()))
					: std::lower_bound(entries.begin(), entries.end(), std::make_pair(chain.exit, std::size_t(0)));
				if (it == entries.end()) it = entries.begin();
				const std::size_t next = it->second;
				if (chains[next].used && next != first) return false;

				double span = chains[next].entry - chain.exit;
				if (span < 0 || (span == 0 && chain.touchExit)) span += rc.perimeter;
				std::pair<double, int> passed[4];
				int numPassed = 0;
				for (int k = 0; k < 4; k++) {
					double d = rc.cornerPos(k) - chain.exit;
					if (d < 0) d += rc.perimeter;
					if (d > 0 && d < span) passed[numPassed++] = { d, k };
				}
				std::sort(passed, passed + numPassed);
				for (int k = 0; k < numPassed; k++) ring.push_back(rc.corner(passed[k].second));

				if (next == first) break;
				c = next;
			}
			if (!geom::equals(ring.front(), ring.back())) ring.push_back(ring.front());
			if (ring.size() < 4 || geom::area(ring) <= 0) return false;
			Polygon p;
			p.outer() = std::move(ring);
			out.push_back(std::move(p));
		}
	}

	// Holes wholly inside the box go to whichever new outer contains them
	for (const Ring *hole : insideHoles) {
		bool placed = false;
		for (std::size_t i = firstOuter; i < out.size() && !placed; i++) {
			const int r = ringInRing(*hole, out[i].outer());
			if (r == 0) return false;
			if (r > 0) {
				out[i].inners().push_back(*hole);
				placed = true;
			}
		}
		if (!placed) return false;
	}
	return true;
}

} // namespace

bool rect_clip(const MultiPolygon &mp, Box const &bbox, MultiPolygon &out) {
	out.clear();
	const RectClipper rc(bbox);
	if (!(rc.w > 0 && rc.h > 0)) return false;
	for (const Polygon &poly : mp)
		if (!clipPolygon(rc, poly, out)) return false;
	return true;
}
