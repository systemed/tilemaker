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
#ifdef __SSE2__
#include <emmintrin.h>
#endif

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
	mp = std::move(result);
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
const double kPrecheckGridLoad = 0.25;      // segments per grid cell
const std::size_t kPrecheckGridMin = 512;   // segments for the grid (fewer: sweep), when the chunks overlap too much
const std::size_t kPrecheckGridEntries = 4; // grid entries per segment at most
const std::size_t kPrecheckAllPairs = 32;   // up to this many segments, all pairs are tested
const std::size_t kPrecheckDirectHoles = 12; // up to this many holes, rays are tested against all segments
const std::size_t kPrecheckSweepHoles = 1;   // more holes: their first vertices are located in one pass
const std::size_t kPrecheckChunkMin = 33;    // segments for the chunks (fewer: all pairs)
const std::size_t kPrecheckChunk = 16;       // consecutive segments per chunk
const std::size_t kPrecheckChunkPairs = 8;   // overlapping chunk pairs per chunk at most (more: grid/sweep)
const std::size_t kPrecheckBridgeMin = 32;  // points: up to this many, no search for clip bridges first
const std::size_t kPrecheckMonoMin = 4;      // segments of a monotone chunk at least (shorter: up to kPrecheckChunk)

struct PrecheckSeg {
	PrecheckSeg() {} // (not zeroed when the vector grows: ringClear writes every field)
	double minX, maxX, minY, maxY;
	const Point *a; // segment a[0] -> a[1]
	uint32_t ring, idx; // ring number, segment index in the ring | kPrecheckLastSeg for the ring's last one
};
const uint32_t kPrecheckLastSeg = 1u << 31;
struct PrecheckBox {
	double minX, maxX, minY, maxY;
	uint32_t id; // ring or polygon number
};
struct PrecheckSegRange {
	const PrecheckSeg *b, *e;
	std::size_t size() const { return std::size_t(e - b); }
	const PrecheckSeg &operator[](std::size_t i) const { return b[i]; }
	const PrecheckSeg *begin() const { return b; }
	const PrecheckSeg *end() const { return e; }
};
struct PrecheckSegBox {
	double minX, maxXTol, minY, maxYTol; // maxXTol = maxX + tol, maxYTol = maxY + tol
	uint32_t idx;                        // segment (sweep)
	uint32_t cx0, cx1, cy0, cy1;         // grid cells covered (grid)
};
struct PrecheckChunk {
	double minX, maxXTol, minY, maxYTol; // maxXTol = maxX + tol, maxYTol = maxY + tol
	uint32_t b, e;                       // segments [b, e), of one ring
	bool monotone;                       // no pair within the chunk needs testing
};
struct PrecheckQuery {
	double y;
	uint32_t hole;
};
struct PrecheckEdgeSeg {
	uint32_t edge;     // which line of the clip box the segment lies on
	double lo, hi;     // its extent along that line
	uint32_t ring, idx; // ring number in the polygon, position among the ring's segments of nonzero length
};
struct PrecheckScratch {
	std::vector<PrecheckSeg> segs;
	std::vector<PrecheckSegBox> segBoxes;
	std::vector<double> tols;
	std::vector<std::size_t> polyStart;
	std::vector<uint32_t> bucketStart;
	std::vector<PrecheckBox> boxes;
	std::vector<std::pair<uint32_t, uint32_t>> pairs;
	std::vector<uint32_t> bandStart, bandFill, bandSegs, crossed;
	std::vector<PrecheckQuery> queries;
	std::vector<uint8_t> unsure;
	std::vector<PrecheckChunk> chunks;
	std::vector<std::pair<uint32_t, uint32_t>> chunkPairs;
	std::vector<PrecheckEdgeSeg> edgeSegs;
	void trim() {
		// Every vector is trimmed after each call. With few segments, only the vectors whose
		// size is not bounded by about 4x the segments (pairs of polygons) can have
		// grown past kPrecheckRetained, so the others need no test.
		if (segs.capacity() <= kPrecheckRetained / 8 && pairs.capacity() <= kPrecheckRetained &&
		    edgeSegs.capacity() <= kPrecheckRetained && crossed.capacity() <= kPrecheckRetained) return;
		if (chunks.capacity() > kPrecheckRetained) std::vector<PrecheckChunk>().swap(chunks);
		if (edgeSegs.capacity() > kPrecheckRetained) std::vector<PrecheckEdgeSeg>().swap(edgeSegs);
		if (chunkPairs.capacity() > kPrecheckRetained) std::vector<std::pair<uint32_t, uint32_t>>().swap(chunkPairs);
		if (queries.capacity() > kPrecheckRetained) std::vector<PrecheckQuery>().swap(queries);
		if (unsure.capacity() > kPrecheckRetained) std::vector<uint8_t>().swap(unsure);
		if (segs.capacity() > kPrecheckRetained) std::vector<PrecheckSeg>().swap(segs);
		if (segBoxes.capacity() > kPrecheckRetained) std::vector<PrecheckSegBox>().swap(segBoxes);
		if (tols.capacity() > kPrecheckRetained) std::vector<double>().swap(tols);
		if (polyStart.capacity() > kPrecheckRetained) std::vector<std::size_t>().swap(polyStart);
		if (bucketStart.capacity() > kPrecheckRetained) std::vector<uint32_t>().swap(bucketStart);
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

// clearOrient runs in every hot loop; GCC does not inline it on its own
#if defined(__GNUC__)
#define PRECHECK_ALWAYS_INLINE __attribute__((always_inline)) inline
#else
#define PRECHECK_ALWAYS_INLINE inline
#endif

// Orientation of c relative to a->b: +1 counter-clockwise, -1 clockwise, 0 when too close to call.
PRECHECK_ALWAYS_INLINE int clearOrient(const Point &a, const Point &b, const Point &c, double tol) {
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

// Set by multiPolygonDefinitelyValidAfterClip: segment pairs are decided as Boost decides sides
thread_local bool precheckBoostSides = false;
thread_local bool precheckUsedBoostSides = false;

// Set by the ...OrReversed entry points: the orientation every ring must have. 0 = as Boost
// expects (the plain precheck), 2 = not decided yet (the first ring decides), 1 = as Boost
// expects, -1 = reversed (every ring clearly has the opposite orientation).
thread_local int precheckOrient = 0;

// Side of c relative to a->b as Boost's side_by_triangle decides it: the sign of the
// determinant unless it is within rounding error or Boost's zero band (0 = can't tell).
inline int boostSide(const Point &a, const Point &b, const Point &c) {
	const double dx = b.x() - a.x(), dy = b.y() - a.y();
	const double px = c.x() - a.x(), py = c.y() - a.y();
	const double qx = c.x() - b.x(), qy = c.y() - b.y();
	const double t1 = dx * py, t2 = dy * px;
	const double d = t1 - t2;
	const double f = std::max(max3(1.0, std::fabs(dx), std::fabs(dy)),
	                          std::max(max3(std::fabs(px), std::fabs(py), std::fabs(qx)), std::fabs(qy)));
	const double bound = std::max(8 * std::numeric_limits<double>::epsilon() * (std::fabs(t1) + std::fabs(t2)),
	                              64 * std::numeric_limits<double>::epsilon() * f * f);
	if (std::fabs(d) > bound) return d > 0 ? 1 : -1;
	return 0;
}

// Coordinates finite; accumulates the largest magnitude (maxAbs is only meaningful when true:
// over finite values, the maximum does not depend on the order it is taken in).
PRECHECK_ALWAYS_INLINE bool ringFinite(const Ring &r, double &maxAbs) {
	const std::size_t n = r.size();
#ifdef __SSE2__
	static_assert(sizeof(Point) == 2 * sizeof(double), "Point must be two packed doubles");
	const double *d = reinterpret_cast<const double*>(r.data());
	const __m128d absMask = _mm_castsi128_pd(_mm_set1_epi64x(0x7fffffffffffffffLL));
	const __m128d big = _mm_set1_pd(std::numeric_limits<double>::max());
	__m128d mx0 = _mm_set1_pd(maxAbs), mx1 = mx0;
	__m128d ok0 = _mm_castsi128_pd(_mm_set1_epi32(-1)), ok1 = ok0;
	std::size_t i = 0;
	for (; i + 2 <= n; i += 2) {
		const __m128d v0 = _mm_and_pd(_mm_loadu_pd(d + 2 * i), absMask);
		const __m128d v1 = _mm_and_pd(_mm_loadu_pd(d + 2 * i + 2), absMask);
		ok0 = _mm_and_pd(ok0, _mm_cmple_pd(v0, big)); // false for inf and NaN
		ok1 = _mm_and_pd(ok1, _mm_cmple_pd(v1, big));
		mx0 = _mm_max_pd(mx0, v0); mx1 = _mm_max_pd(mx1, v1);
	}
	if (i < n) {
		const __m128d v0 = _mm_and_pd(_mm_loadu_pd(d + 2 * i), absMask);
		ok0 = _mm_and_pd(ok0, _mm_cmple_pd(v0, big));
		mx0 = _mm_max_pd(mx0, v0);
	}
	if (_mm_movemask_pd(_mm_and_pd(ok0, ok1)) != 3) return false;
	mx0 = _mm_max_pd(mx0, mx1);
	maxAbs = std::max(_mm_cvtsd_f64(mx0), _mm_cvtsd_f64(_mm_unpackhi_pd(mx0, mx0)));
	return true;
#else
	for (const Point &p : r) {
		const double x = p.x(), y = p.y();
		if (!std::isfinite(x) || !std::isfinite(y)) return false;
		maxAbs = max3(maxAbs, std::fabs(x), std::fabs(y));
	}
	return true;
#endif
}

// No spike at b between segments a->b and b->c: they only share b unless they fold back.
PRECHECK_ALWAYS_INLINE bool vertexClear(const Point &a, const Point &b, const Point &c, double tol) {
	if (clearOrient(a, b, c, tol) != 0) return true;
	// (nearly) collinear: must clearly continue forward
	const double t1 = (b.x() - a.x()) * (c.x() - b.x());
	const double t2 = (b.y() - a.y()) * (c.y() - b.y());
	if (t1 + t2 > kPrecheckEps * (std::fabs(t1) + std::fabs(t2))) return true;
	// Folding back at b (a needle): no spike for Boost as long as it can tell c's side of a->b
	if (!precheckBoostSides || boostSide(a, b, c) == 0) return false;
	precheckUsedBoostSides = true;
	return true;
}

// The area of a ring (doubled, from ringClear's sums over its n points) must clearly exceed both
// our rounding error and that of Boost's sum, with the orientation wanted (see precheckOrient).
PRECHECK_ALWAYS_INLINE bool ringAreaClear(double area, double areaScale, double boostScale, std::size_t n, bool interior) {
	const double relEps = std::max(kPrecheckEps, 8.0 * (n + 4) * std::numeric_limits<double>::epsilon());
	if (!(std::fabs(area) > relEps * areaScale && std::fabs(area) > relEps * boostScale)) return false;
	const bool clockwise = boost::geometry::point_order<Polygon>::value == boost::geometry::clockwise;
	const bool wantPositive = clockwise == interior; // clockwise polygon: CW outer, CCW inners
	const bool reversed = wantPositive ? area <= 0 : area >= 0; // area is not 0 here
	if (precheckOrient == 0) {
		if (reversed) return false;
	} else {
		const int o = reversed ? -1 : 1;
		if (precheckOrient == 2) precheckOrient = o;
		else if (precheckOrient != o) return false;
	}
	return true;
}

// Single-ring tests: closed, >= 4 points, consecutive points either exactly
// equal (Boost allows duplicate points; such zero-length segments are skipped)
// or clearly distinct, area clearly non-zero with the orientation Boost
// expects, no spikes. Appends the ring's segments (the caller has reserved a
// segment per point).
template <class Segs>
bool ringClear(const Ring &r, bool interior, double tol, uint32_t ringIdx, Segs &segs) {
	const std::size_t n = r.size();
	if (n < 4 || n >= std::numeric_limits<uint32_t>::max()) return false;
	const std::size_t m = n - 1; // segments: i runs r[i] -> r[i+1]
	if (r[0].x() != r[m].x() || r[0].y() != r[m].y()) return false;
	// (segment indexes below kPrecheckLastSeg: rings of 2^31 points or more are left to Boost)
	if (n > kPrecheckLastSeg) return false;

	const std::size_t first = segs.size();
	segs.resize(first + m); // (callers reserve a segment per point: no reallocation)
	PrecheckSeg *out = segs.data() + first;
	uint32_t count = 0;
	// Signed area (doubled) relative to r[0], positive = counter-clockwise, and the scales of its
	// rounding error and of Boost's (x1+x2)*(y1-y2) sum, accumulated in the same pass (each sum
	// in the order of i, as separate loops would)
	const double x0 = r[0].x(), y0 = r[0].y();
	double area = 0, areaScale = 0, boostScale = 0;
	const Point *prev = nullptr; // start of the previous segment
	for (std::size_t i = 0; i < m; i++) {
		const Point &a = r[i], &b = r[i + 1];
		boostScale += (std::fabs(a.x()) + std::fabs(b.x())) * std::fabs(a.y() - b.y());
		if (i >= 1 && i + 1 < m) {
			const double t1 = (a.x() - x0) * (b.y() - y0);
			const double t2 = (a.y() - y0) * (b.x() - x0);
			area += t1 - t2;
			areaScale += std::fabs(t1) + std::fabs(t2);
		}
		if (a.x() == b.x() && a.y() == b.y()) continue;
		if (std::fabs(b.x() - a.x()) <= tol && std::fabs(b.y() - a.y()) <= tol) return false;
		PrecheckSeg &s = out[count];
		s.minX = std::min(a.x(), b.x()); s.maxX = std::max(a.x(), b.x());
		s.minY = std::min(a.y(), b.y()); s.maxY = std::max(a.y(), b.y());
		s.a = &a;
		s.ring = ringIdx; s.idx = count++;
		// the vertex between the previous segment and this one (the first segment's start: below)
		if (prev && !vertexClear(*prev, a, b, tol)) return false;
		prev = &a;
	}
	segs.resize(first + count);
	if (count < 3) return false;
	if (!vertexClear(*prev, out[0].a[0], out[0].a[1], tol)) return false;
	out[count - 1].idx |= kPrecheckLastSeg;
	return ringAreaClear(area, areaScale, boostScale, n, interior);
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

// The side tests of segsClear: segments p1->p2 and q1->q2 are clearly apart.
PRECHECK_ALWAYS_INLINE bool sidesClear(const Point &p1, const Point &p2, const Point &q1, const Point &q2, double tol) {
	const int o1 = clearOrient(p1, p2, q1, tol), o2 = clearOrient(p1, p2, q2, tol);
	if (o1 != 0 && o1 == o2) return true; // q entirely on one side of line p
	const int o3 = clearOrient(q1, q2, p1, tol), o4 = clearOrient(q1, q2, p2, tol);
	if (o3 != 0 && o3 == o4) return true; // p entirely on one side of line q
	if (!precheckBoostSides) return false; // crossing, touching or unsure
	// Apart as Boost would find them: one segment clearly on one side of the other's line
	const int b1 = boostSide(p1, p2, q1), b2 = boostSide(p1, p2, q2);
	const int b3 = boostSide(q1, q2, p1), b4 = boostSide(q1, q2, p2);
	if ((b1 != 0 && b1 == b2) || (b3 != 0 && b3 == b4)) { precheckUsedBoostSides = true; return true; }
	return false;
}

// Two segments are clearly disjoint (adjacent segments of a ring are handled by the spike test).
inline bool segsClear(const PrecheckSeg &s, const PrecheckSeg &t, double tol) {
	if (s.ring == t.ring) {
		// adjacent: consecutive, or the first and the last segment of the ring
		const uint32_t si = s.idx & ~kPrecheckLastSeg, ti = t.idx & ~kPrecheckLastSeg;
		const uint32_t lo = std::min(si, ti), hi = std::max(si, ti);
		if (hi - lo == 1 || (lo == 0 && ((s.idx | t.idx) & kPrecheckLastSeg))) return true;
	}
	if (s.maxX + tol < t.minX || t.maxX + tol < s.minX || s.maxY + tol < t.minY || t.maxY + tol < s.minY)
		return true;
	return sidesClear(s.a[0], s.a[1], t.a[0], t.a[1], tol);
}

// Pairs whose boxes are clearly apart are clear whatever else segsClear tests (its box test
// comes before anything that could fail), so they can be skipped by that box test first.
inline bool boxesClearlyApart(const PrecheckSeg &s, const PrecheckSeg &t, double tol) {
	return (s.maxX + tol < t.minX) | (t.maxX + tol < s.minX) | (s.maxY + tol < t.minY) | (t.maxY + tol < s.minY);
}

// Uniform grid over the segments' boxes: each segment is entered in every cell its box
// [minX, maxX + tol] x [minY, maxY + tol] covers (cellX/cellY are monotone, so two boxes
// that are not clearly apart share a cell), and a pair is tested only in the cell at the low
// corner of the overlap of their cell ranges, so exactly once. 1 = all clear, 0 = not,
// -1 = the grid would get too big (long segments), use the sweep.
int segmentsClearGrid(const std::vector<PrecheckSeg> &segs, PrecheckScratch &scratch, double tol) {
	const std::size_t n = segs.size();
	std::vector<PrecheckSegBox> &gb = scratch.segBoxes;
	gb.resize(n);
	double x0 = std::numeric_limits<double>::infinity(), x1 = -x0, y0 = x0, y1 = -x0;
	for (std::size_t i = 0; i < n; i++) {
		const PrecheckSeg &s = segs[i];
		PrecheckSegBox &b = gb[i];
		b.minX = s.minX; b.maxXTol = s.maxX + tol; b.minY = s.minY; b.maxYTol = s.maxY + tol;
		x0 = std::min(x0, b.minX); x1 = std::max(x1, b.maxXTol);
		y0 = std::min(y0, b.minY); y1 = std::max(y1, b.maxYTol);
	}
	// about kPrecheckGridLoad segments per cell, cells roughly square
	const double w = x1 - x0, h = y1 - y0;
	const double cells = std::max(1.0, double(n) / kPrecheckGridLoad);
	double fx = w > 0 && h > 0 ? std::sqrt(cells * w / h) : (w > 0 ? cells : 1.0);
	fx = std::min(std::max(fx, 1.0), cells);
	const std::size_t nx = static_cast<std::size_t>(fx);
	const std::size_t ny = std::max<std::size_t>(1, static_cast<std::size_t>(cells / double(nx)));
	const double sx = w > 0 ? double(nx) / w : 0.0, sy = h > 0 ? double(ny) / h : 0.0;
	const double lastX = double(nx - 1), lastY = double(ny - 1);
	auto cellX = [&](double x) {
		const double c = (x - x0) * sx;
		return c >= lastX ? static_cast<uint32_t>(nx - 1) : c <= 0 ? 0u : static_cast<uint32_t>(c);
	};
	auto cellY = [&](double y) {
		const double c = (y - y0) * sy;
		return c >= lastY ? static_cast<uint32_t>(ny - 1) : c <= 0 ? 0u : static_cast<uint32_t>(c);
	};
	std::size_t entries = 0;
	for (PrecheckSegBox &b : gb) {
		b.cx0 = cellX(b.minX); b.cy0 = cellY(b.minY);
		b.cx1 = cellX(b.maxXTol); b.cy1 = cellY(b.maxYTol);
		entries += std::size_t(b.cx1 - b.cx0 + 1) * (b.cy1 - b.cy0 + 1);
	}
	if (entries > kPrecheckGridEntries * n) return -1;

	const std::size_t numCells = nx * ny;
	std::vector<uint32_t> &start = scratch.bucketStart, &fill = scratch.bandFill, &items = scratch.bandSegs;
	start.assign(numCells + 1, 0);
	for (const PrecheckSegBox &b : gb)
		for (uint32_t cy = b.cy0; cy <= b.cy1; cy++)
			for (uint32_t cx = b.cx0; cx <= b.cx1; cx++) start[cy * nx + cx + 1]++;
	for (std::size_t c = 0; c < numCells; c++) start[c + 1] += start[c];
	fill.assign(start.begin(), start.end() - 1);
	items.resize(entries);
	for (std::size_t i = 0; i < n; i++) {
		const PrecheckSegBox &b = gb[i];
		for (uint32_t cy = b.cy0; cy <= b.cy1; cy++)
			for (uint32_t cx = b.cx0; cx <= b.cx1; cx++) items[fill[cy * nx + cx]++] = static_cast<uint32_t>(i);
	}
	for (std::size_t cy = 0; cy < ny; cy++)
		for (std::size_t cx = 0; cx < nx; cx++) {
			const std::size_t c = cy * nx + cx;
			for (uint32_t a = start[c]; a < start[c + 1]; a++) {
				const PrecheckSegBox bi = gb[items[a]];
				for (uint32_t k = a + 1; k < start[c + 1]; k++) {
					const PrecheckSegBox &bj = gb[items[k]];
					// the box test of segsClear: s.maxX + tol < t.minX || t.maxX + tol < s.minX || (same in y)
					if ((bi.maxXTol < bj.minX) | (bj.maxXTol < bi.minX) | (bi.maxYTol < bj.minY) | (bj.maxYTol < bi.minY)) continue;
					if (std::max(bi.cx0, bj.cx0) != cx || std::max(bi.cy0, bj.cy0) != cy) continue; // tested in another cell
					if (!segsClear(segs[items[a]], segs[items[k]], tol)) return 0;
				}
			}
		}
	return 1;
}

// Chunks of consecutive segments (rings are spatially coherent, so a chunk's box is small):
// a pair of segments from two chunks whose boxes are clearly apart is clearly apart too
// (s.maxX <= A.maxX and t.minX >= B.minX, and adding tol is monotone), so only the pairs
// within a chunk and between chunks whose boxes are not clearly apart are tested. 1 = all
// clear, 0 = not, -1 = the chunks overlap too much (long or folded segments), use the grid/sweep.
int segmentsClearChunked(const std::vector<PrecheckSeg> &segs, PrecheckScratch &scratch, double tol) {
	const std::size_t n = segs.size();
	const std::size_t K = kPrecheckChunk;
	// up to K consecutive segments of one ring per chunk (a chunk across two rings would be large)
	std::vector<PrecheckChunk> &cb = scratch.chunks;
	cb.clear();
	double sumW = 0, sumH = 0, allX0 = std::numeric_limits<double>::infinity(), allX1 = -allX0, allY0 = allX0, allY1 = -allX0;
	for (std::size_t b = 0; b < n; ) {
		std::size_t e = b + 1;
		double x0 = segs[b].minX, x1 = segs[b].maxX, y0 = segs[b].minY, y1 = segs[b].maxY;
		// Monotone chunk: the chunk's points p0, p1, ... (segment k runs p_k -> p_k+1) increase in x,
		// p_k + tol < p_k+1 for every segment but the first, which only must not decrease. Then
		// every pair of segments i < j - 1 is clearly apart in x as the box test computes it:
		// s.maxX + tol = p_i+1 + tol < p_i+2 <= p_j = t.minX (rounding is monotone). Likewise
		// decreasing (t.maxX + tol <= p_i+2 + tol < p_i+1 = s.minX) and in y.
		const Point *pb = segs[b].a;
		unsigned mono = (pb[0].x() <= pb[1].x() ? 1u : 0u) | (pb[0].x() >= pb[1].x() ? 2u : 0u)
		              | (pb[0].y() <= pb[1].y() ? 4u : 0u) | (pb[0].y() >= pb[1].y() ? 8u : 0u);
		while (e < n && e - b < K && segs[e].ring == segs[b].ring) {
			const PrecheckSeg &s = segs[e];
			const bool wideX = s.minX + tol < s.maxX, wideY = s.minY + tol < s.maxY;
			const bool incX = s.a[0].x() < s.a[1].x(), incY = s.a[0].y() < s.a[1].y();
			const unsigned next = mono & ((wideX && incX ? 1u : 0u) | (wideX && !incX ? 2u : 0u) | (wideY && incY ? 4u : 0u) | (wideY && !incY ? 8u : 0u));
			// a monotone chunk of a few segments ends where it would stop being monotone
			if (!next && mono && e - b >= kPrecheckMonoMin) break;
			mono = next;
			x0 = std::min(x0, s.minX); x1 = std::max(x1, s.maxX);
			y0 = std::min(y0, s.minY); y1 = std::max(y1, s.maxY);
			e++;
		}
		cb.push_back({ x0, x1 + tol, y0, y1 + tol, static_cast<uint32_t>(b), static_cast<uint32_t>(e), mono != 0 });
		sumW += x1 - x0; sumH += y1 - y0;
		allX0 = std::min(allX0, x0); allX1 = std::max(allX1, x1); allY0 = std::min(allY0, y0); allY1 = std::max(allY1, y1);
		b = e;
	}
	const std::size_t m = cb.size();
	// Sweep along the axis in which the chunks are narrower relative to the whole extent
	// (fewer chunks in each sweep window). Which pairs are found does not depend on it.
	const bool sweepY = sumH * (allX1 - allX0) < sumW * (allY1 - allY0);
	if (sweepY) std::sort(cb.begin(), cb.end(), [](const PrecheckChunk &a, const PrecheckChunk &b) { return a.minY < b.minY; });
	else std::sort(cb.begin(), cb.end(), [](const PrecheckChunk &a, const PrecheckChunk &b) { return a.minX < b.minX; });
	// Overlapping chunk pairs first (positions in cb): bail out before testing anything if there are too many
	std::vector<std::pair<uint32_t, uint32_t>> &pairs = scratch.chunkPairs;
	pairs.clear();
	const std::size_t maxPairs = kPrecheckChunkPairs * m;
	for (std::size_t i = 0; i < m; i++) {
		const PrecheckChunk bi = cb[i];
		const double limit = sweepY ? bi.maxYTol : bi.maxXTol;
		for (std::size_t j = i + 1; j < m && (sweepY ? cb[j].minY : cb[j].minX) <= limit; j++) {
			const PrecheckChunk &bj = cb[j];
			if ((bi.maxXTol < bj.minX) | (bj.maxXTol < bi.minX) | (bi.maxYTol < bj.minY) | (bj.maxYTol < bi.minY)) continue;
			pairs.emplace_back(static_cast<uint32_t>(i), static_cast<uint32_t>(j));
			if (pairs.size() > maxPairs) return -1;
		}
	}
	// pairs within each chunk; consecutive segments of a ring are adjacent, which segsClear
	// accepts before anything else
	for (const PrecheckChunk &c : cb) {
		if (c.monotone) continue;
		for (std::size_t i = c.b; i < c.e; i++)
			for (std::size_t j = i + 2; j < c.e; j++)
				if (!boxesClearlyApart(segs[i], segs[j], tol) && !segsClear(segs[i], segs[j], tol)) return 0;
	}
	// pairs between overlapping chunks: only segments not clearly apart from the other chunk's box
	// can be in a pair that is not clearly apart (same monotonicity argument)
	uint32_t la[kPrecheckChunk], lb[kPrecheckChunk];
	for (const auto &pr : pairs) {
		const PrecheckChunk &A = cb[pr.first], &B = cb[pr.second];
		std::size_t na = 0, nb = 0;
		for (std::size_t i = A.b; i < A.e; i++) {
			const PrecheckSeg &s = segs[i];
			// s against B: s.maxX + tol < B.minX || B.maxX + tol < s.minX || (same in y)
			if (!((s.maxX + tol < B.minX) | (B.maxXTol < s.minX) | (s.maxY + tol < B.minY) | (B.maxYTol < s.minY))) la[na++] = static_cast<uint32_t>(i);
		}
		if (!na) continue;
		for (std::size_t j = B.b; j < B.e; j++) {
			const PrecheckSeg &t = segs[j];
			if (!((t.maxX + tol < A.minX) | (A.maxXTol < t.minX) | (t.maxY + tol < A.minY) | (A.maxYTol < t.minY))) lb[nb++] = static_cast<uint32_t>(j);
		}
		for (std::size_t a = 0; a < na; a++)
			for (std::size_t b = 0; b < nb; b++)
				if (!boxesClearlyApart(segs[la[a]], segs[lb[b]], tol) && !segsClear(segs[la[a]], segs[lb[b]], tol)) return 0;
	}
	return 1;
}

// All pairs of segments clearly disjoint. segsClear is symmetric and true for every pair whose
// boxes are clearly apart, so the answer only needs every other pair to be tested, in any order:
// all pairs for a few segments, chunks of consecutive segments for many, a grid or else a sweep in x.
template <class Segs>
inline bool allPairsClear(const Segs &segs, double tol) {
	const std::size_t n = segs.size();
	for (std::size_t i = 0; i < n; i++)
		for (std::size_t j = i + 1; j < n; j++)
			if (!boxesClearlyApart(segs[i], segs[j], tol) && !segsClear(segs[i], segs[j], tol)) return false;
	return true;
}

bool segmentsClear(std::vector<PrecheckSeg> &segs, PrecheckScratch &scratch, double tol) {
	const std::size_t n = segs.size();
	if (n <= kPrecheckAllPairs) return allPairsClear(segs, tol);
	if (n >= kPrecheckChunkMin && n < (std::size_t(1) << 30)) {
		const int c = segmentsClearChunked(segs, scratch, tol);
		if (c >= 0) return c == 1;
	}
	if (n >= kPrecheckGridMin && n < (std::size_t(1) << 30)) {
		const int g = segmentsClearGrid(segs, scratch, tol);
		if (g >= 0) return g == 1;
	}
	// Sweep in x without a full sort: the segments are bucketed by minX (counting sort into
	// n buckets; bucketOf is monotone in x). Each segment i is paired with the segments after it
	// in bucket order up to the bucket of its maxX + tol: a segment j that is not clearly apart
	// from i in x has minX[j] <= maxX[i] + tol, so it lies in such a bucket, or else i lies in
	// one of j's. Every pair whose boxes are not clearly apart is thus tested once.
	double minX = std::numeric_limits<double>::infinity(), maxX = -minX;
	for (const PrecheckSeg &s : segs) { minX = std::min(minX, s.minX); maxX = std::max(maxX, s.minX); }
	const double scale = maxX > minX ? double(n) / (maxX - minX) : 0.0;
	const double lastBucket = double(n - 1);
	auto bucketOf = [&](double x) {
		const double b = (x - minX) * scale;
		return b >= lastBucket ? n - 1 : b <= 0 ? std::size_t(0) : static_cast<std::size_t>(b);
	};
	std::vector<uint32_t> &start = scratch.bucketStart;
	start.assign(n + 1, 0);
	for (const PrecheckSeg &s : segs) start[bucketOf(s.minX) + 1]++;
	for (std::size_t b = 0; b < n; b++) start[b + 1] += start[b];
	std::vector<PrecheckSegBox> &sb = scratch.segBoxes;
	sb.resize(n);
	{
		std::vector<uint32_t> &fill = scratch.bandFill;
		fill.assign(start.begin(), start.end() - 1);
		for (std::size_t i = 0; i < n; i++) {
			const PrecheckSeg &s = segs[i];
			PrecheckSegBox &b = sb[fill[bucketOf(s.minX)]++];
			b.minX = s.minX; b.maxXTol = s.maxX + tol; b.minY = s.minY; b.maxYTol = s.maxY + tol;
			b.idx = static_cast<uint32_t>(i);
		}
	}
	for (std::size_t i = 0; i < n; i++) {
		const PrecheckSegBox &bi = sb[i];
		const std::size_t end = start[bucketOf(bi.maxXTol) + 1];
		for (std::size_t j = i + 1; j < end; j++) {
			const PrecheckSegBox &bj = sb[j];
			// the box test of segsClear: s.maxX + tol < t.minX || t.maxX + tol < s.minX || (same in y)
			if ((bi.maxXTol < bj.minX) | (bj.maxXTol < bi.minX) | (bi.maxYTol < bj.minY) | (bj.maxYTol < bi.minY)) continue;
			if (!segsClear(segs[bi.idx], segs[bj.idx], tol)) return false;
		}
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

// Placement test of the holes of p, given forEach(h, f): calls f(segment) for (at least) every
// segment of p whose y range reaches h.y within tol, stopping when f returns false. A ray
// from h never crosses the other segments (rayClearlyCrosses says -1 for them), so the
// answer does not depend on which of those are passed, nor on the order.
template <class ForEach>
bool holesPlacedWith(const Polygon &p, uint32_t ringBase, std::vector<uint32_t> &crossed, double tol, ForEach forEach) {
	const auto &inners = p.inners();
	// 1 = h inside the outer ring and no other hole, -1 = not, 0 = unsure
	auto locate = [&](const Point &h, uint32_t ownRing) {
		crossed.clear();
		bool unsure = false;
		const double hy = h.y();
		forEach(h, [&](const PrecheckSeg &s) {
			// rayClearlyCrosses' first test (exactly one end above hy), on the segment's box
			// (minY/maxY are its ends' y) without loading the points
			if (!(s.minY <= hy && hy < s.maxY)) return true;
			if (s.ring == ownRing) return true;
			const int c = rayClearlyCrosses(h, s.a[0], s.a[1], tol);
			if (c == 0) { unsure = true; return false; }
			if (c > 0) crossed.push_back(s.ring);
			return true;
		});
		if (unsure) return 0;
		std::sort(crossed.begin(), crossed.end());
		bool inOuter = false;
		for (std::size_t i = 0; i < crossed.size(); ) {
			std::size_t j = i;
			while (j < crossed.size() && crossed[j] == crossed[i]) j++;
			if ((j - i) % 2 == 1) {
				if (crossed[i] != ringBase) return -1; // inside another hole
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
			loc = locate(hole[k * m / tries], static_cast<uint32_t>(ringBase + i + 1));
		if (loc != 1) return false;
	}
	return true;
}

// Holes lie inside the outer ring (ring ringBase) and outside each other. `segs` holds the
// segments of all rings of p (numbered ringBase, ringBase + 1, ...), known not to touch.
// They are bucketed into horizontal bands, so that a ray from a vertex of each hole is
// tested against the segments of that vertex's band only, counting crossings per ring (odd = inside that ring).
// With few holes (few rays) the rays are tested against all segments instead.
// The first vertex of every hole located at once (what holesPlacedWith computes for its first
// try per hole): one pass over the segments, each paired with the holes whose first vertex
// lies in its half-open y range [minY, maxY), which are exactly the segments whose crossing
// rayClearlyCrosses decides (all others give -1). 1 = all holes placed, 0 = some hole is not,
// -1 = some first vertex is unsure (and none was found misplaced): run holesPlacedWith.
int holesFirstVerticesPlaced(const Polygon &p, PrecheckSegRange segs, uint32_t ringBase, PrecheckScratch &scratch, double tol) {
	const auto &inners = p.inners();
	const std::size_t q = inners.size();
	std::vector<PrecheckQuery> &qs = scratch.queries;
	qs.resize(q);
	for (std::size_t i = 0; i < q; i++) qs[i] = { inners[i][0].y(), static_cast<uint32_t>(i) };
	std::sort(qs.begin(), qs.end(), [](const PrecheckQuery &a, const PrecheckQuery &b) { return a.y < b.y; });
	// per hole: bit 0 unsure, bit 1 inside the outer ring (parity of its crossings), bit 2 parity of
	// the crossings of all other holes together. That parity is odd exactly when the vertex lies in
	// an odd number of other holes. Per ring parities are not needed: the holes don't touch, so the
	// holes containing a vertex of hole X are nested in a chain H1 inside H2 ... inside Hk, all
	// containing X. If k >= 2 is even, H1's first vertex lies in exactly H2 ... Hk, k - 1 (odd)
	// holes. So when some hole lies in another hole, some hole's parity is odd: if all holes are
	// sure, the answer (0) is as with per ring parities; if that hole is unsure, the answer is -1
	// instead of 0, and holesPlacedWith, which places X's first vertex just as here, says false.
	// An odd parity always means that hole lies in another one (0 as before).
	std::vector<uint8_t> &unsure = scratch.unsure;
	unsure.assign(q, 0);
	std::size_t pos = 0; // first query with y >= minY of the current segment
	for (const PrecheckSeg &s : segs) {
		const double y0 = s.minY, y1 = s.maxY;
		if (!(y0 < y1)) continue; // horizontal: no y is in [minY, maxY)
		// walk from the previous segment's position (rings are coherent), binary search if far
		int steps = 0;
		while (pos > 0 && qs[pos - 1].y >= y0 && steps < 8) { pos--; steps++; }
		while (pos < q && qs[pos].y < y0 && steps < 8) { pos++; steps++; }
		if (steps == 8)
			pos = std::lower_bound(qs.begin(), qs.end(), y0, [](const PrecheckQuery &a, double y) { return a.y < y; }) - qs.begin();
		for (std::size_t k = pos; k < q && qs[k].y < y1; k++) {
			const uint32_t hole = qs[k].hole;
			if (s.ring == ringBase + hole + 1) continue;
			const int c = rayClearlyCrosses(inners[hole][0], s.a[0], s.a[1], tol);
			if (c == 0) unsure[hole] |= 1;
			else if (c > 0) unsure[hole] ^= s.ring == ringBase ? 2 : 4;
		}
	}
	// per hole: inside the outer ring (odd crossings of ringBase) and in no other hole
	bool anyUnsure = false;
	for (uint32_t hole = 0; hole < q; hole++) {
		if (unsure[hole] & 1) { anyUnsure = true; continue; }
		if ((unsure[hole] & 6) != 2) return 0;
	}
	return anyUnsure ? -1 : 1;
}

bool holesClearlyPlaced(const Polygon &p, PrecheckSegRange segs, uint32_t ringBase, PrecheckScratch &scratch, double tol) {
	const auto &inners = p.inners();
	if (inners.empty()) return true;
	if (segs.size() >= std::numeric_limits<uint32_t>::max()) return false;
	if (inners.size() > kPrecheckSweepHoles && segs.size() < (std::size_t(1) << 30)) {
		const int r = holesFirstVerticesPlaced(p, segs, ringBase, scratch, tol);
		if (r >= 0) return r == 1;
	}
	// (the band index below fails only with >= 2^32 entries, at most 4 per segment)
	if (inners.size() <= kPrecheckDirectHoles && segs.size() < (std::size_t(1) << 30))
		return holesPlacedWith(p, ringBase, scratch.crossed, tol, [&](const Point &, auto f) {
			for (const PrecheckSeg &s : segs) if (!f(s)) return;
		});

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

	return holesPlacedWith(p, ringBase, scratch.crossed, tol, [&](const Point &h, auto f) {
		const std::size_t b = bandOf(h.y());
		for (uint32_t k = start[b]; k < start[b + 1]; k++)
			if (!f(segs[items[k]])) return;
	});
}

// polygonClear of a polygon of one ring of at most kPrecheckAllPairs segments, on arrays: the
// tests of ringFinite, ringClear and smallPairsClear (all must pass, so their order does not
// matter), each on the same values. Segments of nonzero length start at r[st[j]].
bool smallRingClear(const Ring &r) {
	const std::size_t n = r.size();
	double maxAbs = 0;
	if (!ringFinite(r, maxAbs)) return false;
	const double tol = kPrecheckEps * std::max(1.0, maxAbs);
	if (n < 4) return false;
	const std::size_t m = n - 1; // segments: i runs r[i] -> r[i+1]
	if (r[0].x() != r[m].x() || r[0].y() != r[m].y()) return false;
	const Point *pt = r.data();
	const double x0 = pt[0].x(), y0 = pt[0].y();
	double area = 0, areaScale = 0, boostScale = 0;
	uint32_t st[kPrecheckAllPairs];
	// segment boxes as segsClear's box test uses them (max + tol)
	double bx0[kPrecheckAllPairs], bx1[kPrecheckAllPairs], by0[kPrecheckAllPairs], by1[kPrecheckAllPairs];
	std::size_t k = 0;
	const Point *prev = nullptr; // start of the previous segment
	for (std::size_t i = 0; i < m; i++) {
		const Point &a = pt[i], &b = pt[i + 1];
		boostScale += (std::fabs(a.x()) + std::fabs(b.x())) * std::fabs(a.y() - b.y());
		if (i >= 1 && i + 1 < m) {
			const double t1 = (a.x() - x0) * (b.y() - y0);
			const double t2 = (a.y() - y0) * (b.x() - x0);
			area += t1 - t2;
			areaScale += std::fabs(t1) + std::fabs(t2);
		}
		if (a.x() == b.x() && a.y() == b.y()) continue;
		if (std::fabs(b.x() - a.x()) <= tol && std::fabs(b.y() - a.y()) <= tol) return false;
		if (prev && !vertexClear(*prev, a, b, tol)) return false;
		prev = &a;
		st[k] = static_cast<uint32_t>(i);
		bx0[k] = std::min(a.x(), b.x()); bx1[k] = std::max(a.x(), b.x()) + tol;
		by0[k] = std::min(a.y(), b.y()); by1[k] = std::max(a.y(), b.y()) + tol;
		k++;
	}
	if (k < 3) return false;
	if (!vertexClear(*prev, pt[st[0]], pt[st[0] + 1], tol)) return false;
	if (!ringAreaClear(area, areaScale, boostScale, n, false)) return false;

	// smallPairsClear: every pair but the adjacent ones (segsClear accepts those first), box test
	// first (segsClear's), then segsClear's side tests
	uint8_t cand[kPrecheckAllPairs];
	for (std::size_t i = 0; i + 2 < k; i++) {
		const double ax0 = bx0[i], ax1 = bx1[i], ay0 = by0[i], ay1 = by1[i];
		std::size_t c = 0;
		for (std::size_t j = i + 2, end = i == 0 ? k - 1 : k; j < end; j++) {
			const bool apart = (ax1 < bx0[j]) | (bx1[j] < ax0) | (ay1 < by0[j]) | (by1[j] < ay0);
			cand[c] = static_cast<uint8_t>(j);
			c += !apart;
		}
		for (std::size_t q = 0; q < c; q++) {
			const uint32_t j = st[cand[q]];
			if (!sidesClear(pt[st[i]], pt[st[i] + 1], pt[j], pt[j + 1], tol)) return false;
		}
	}
	return true;
}

bool polygonClear(const Polygon &p, PrecheckScratch &scratch) {
	const Ring &outer = p.outer();
	const auto &inners = p.inners();
	// (the small path does polygonClear's tests below for one ring: ringFinite, then smallPairsClear)
	if (inners.empty() && outer.size() <= kPrecheckAllPairs + 1) return smallRingClear(outer);
	if (inners.size() >= std::numeric_limits<uint32_t>::max()) return false;
	double maxAbs = 0;
	if (!ringFinite(outer, maxAbs)) return false;
	for (const auto &inner : inners)
		if (!ringFinite(inner, maxAbs)) return false;
	const double tol = kPrecheckEps * std::max(1.0, maxAbs);

	std::vector<PrecheckSeg> &segs = scratch.segs;
	segs.clear();
	{
		std::size_t points = outer.size();
		for (const auto &inner : inners) points += inner.size();
		segs.reserve(points); // at most one segment per point: no regrowth
	}
	if (!ringClear(outer, false, tol, 0, segs)) return false;
	for (std::size_t i = 0; i < inners.size(); i++)
		if (!ringClear(inners[i], true, tol, static_cast<uint32_t>(i + 1), segs)) return false;

	// No two rings touch, and no ring touches itself.
	if (!segmentsClear(segs, scratch, tol)) return false;

	// Holes lie inside the outer ring and are not nested in each other.
	return holesClearlyPlaced(p, PrecheckSegRange{ segs.data(), segs.data() + segs.size() }, 0, scratch, tol);
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
	if (mp.size() == 1) return polygonClear(mp[0], scratch);
	if (mp.size() >= std::numeric_limits<uint32_t>::max()) return false;

	// Polygons whose bboxes (outer rings; holes lie inside them) overlap get the pairwise tests below.
	std::vector<PrecheckBox> &boxes = scratch.boxes;
	boxes.clear();
	double maxAbs = 0;
	for (const auto &p : mp) {
		if (p.outer().empty()) return false;
		PrecheckBox box;
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

	if (pairs.empty()) {
		for (const auto &p : mp)
			if (!polygonClear(p, scratch)) return false;
		return true;
	}

	// No ring of one polygon touches a ring of another: all rings are tested together at the
	// multipolygon's tolerance tol. That includes the rings within each polygon, which
	// polygonClear tests at the polygon's own tolerance tolP. When tolP <= tol, the combined
	// tests imply those (every tolerance-dependent decision only gets stricter with a larger
	// tolerance, and the segments are the same), so only the holes are placed per polygon.
	std::vector<double> &tols = scratch.tols;
	tols.clear();
	bool implied = true;
	for (const auto &p : mp) {
		if (p.inners().size() >= std::numeric_limits<uint32_t>::max()) return false;
		double polyMaxAbs = 0;
		if (!ringFinite(p.outer(), polyMaxAbs)) return false;
		for (const auto &inner : p.inners())
			if (!ringFinite(inner, polyMaxAbs)) return false;
		const double tolP = kPrecheckEps * std::max(1.0, polyMaxAbs);
		tols.push_back(tolP);
		if (!(tolP <= tol)) implied = false;
	}
	if (!implied)
		for (const auto &p : mp)
			if (!polygonClear(p, scratch)) return false;

	std::vector<PrecheckSeg> &segs = scratch.segs;
	segs.clear();
	{
		std::size_t points = 0;
		for (const auto &p : mp) {
			points += p.outer().size();
			for (const auto &inner : p.inners()) points += inner.size();
		}
		segs.reserve(points); // at most one segment per point: no regrowth
	}
	std::vector<std::size_t> &polyStart = scratch.polyStart;
	polyStart.clear();
	uint32_t ringIdx = 0;
	for (const auto &p : mp) {
		polyStart.push_back(segs.size());
		if (!ringClear(p.outer(), false, tol, ringIdx++, segs)) return false;
		for (const auto &inner : p.inners())
			if (!ringClear(inner, true, tol, ringIdx++, segs)) return false;
	}
	polyStart.push_back(segs.size());
	if (!segmentsClear(segs, scratch, tol)) return false;

	// Holes lie inside their outer ring and are not nested in each other.
	if (implied) {
		ringIdx = 0;
		for (std::size_t i = 0; i < mp.size(); i++) {
			const PrecheckSegRange range{ segs.data() + polyStart[i], segs.data() + polyStart[i + 1] };
			if (!holesClearlyPlaced(mp[i], range, ringIdx, scratch, tols[i])) return false;
			ringIdx += 1 + static_cast<uint32_t>(mp[i].inners().size());
		}
	}

	// Interiors are disjoint: neither polygon of an overlapping pair lies inside the other.
	for (const auto &pr : pairs)
		if (!polygonClearlyOutside(mp[pr.first], mp[pr.second], tol) ||
		    !polygonClearlyOutside(mp[pr.second], mp[pr.first], tol)) return false;
	return true;
}

// fast_clip leaves zero-width bridges along the box edge where a ring leaves and re-enters the
// box: two segments of one polygon on the same edge line of the box that overlap or touch, and
// are not adjacent in their ring. True when mp has such a pair; then multiPolygonClear is false:
// every pair of segments within a polygon gets segsClear (whichever path, at some tolerance),
// numbered as ringClear numbers them (segments of nonzero length, position in the ring, the
// last adjacent to the first). For two segments on one axis-parallel line, every determinant of
// clearOrient and boostSide is exactly 0 (a zero coordinate difference times another), and their
// boxes are not apart (the extents along the line meet, and tol >= 0 only widens them), so
// segsClear is false at any tolerance, in either mode. (A ring that ringClear rejects first
// makes the answer false as well.)
bool clipBridged(const MultiPolygon &mp, const Box &box, PrecheckScratch &scratch) {
	const double x0 = box.min_corner().x(), x1 = box.max_corner().x();
	const double y0 = box.min_corner().y(), y1 = box.max_corner().y();
	std::vector<PrecheckEdgeSeg> &es = scratch.edgeSegs;
	std::vector<uint32_t> &counts = scratch.crossed; // segments of nonzero length per ring
	for (const Polygon &p : mp) {
		es.clear();
		counts.clear();
		const std::size_t rings = 1 + p.inners().size();
		for (std::size_t ri = 0; ri < rings; ri++) {
			const Ring &r = ri == 0 ? p.outer() : p.inners()[ri - 1];
			uint32_t idx = 0;
			for (std::size_t i = 0; i + 1 < r.size(); i++) {
				const Point &a = r[i], &b = r[i + 1];
				const double ax = a.x(), ay = a.y(), bx = b.x(), by = b.y();
				if (ax == bx && ay == by) continue;
				if (ax == bx && (ax == x0 || ax == x1))
					es.push_back({ ax == x0 ? 0u : 1u, std::min(ay, by), std::max(ay, by), static_cast<uint32_t>(ri), idx });
				else if (ay == by && (ay == y0 || ay == y1))
					es.push_back({ ay == y0 ? 2u : 3u, std::min(ax, bx), std::max(ax, bx), static_cast<uint32_t>(ri), idx });
				idx++;
			}
			counts.push_back(idx);
		}
		if (es.size() < 2) continue;
		std::sort(es.begin(), es.end(), [](const PrecheckEdgeSeg &a, const PrecheckEdgeSeg &b) {
			return a.edge != b.edge ? a.edge < b.edge : a.lo < b.lo;
		});
		for (std::size_t i = 0; i < es.size(); i++)
			for (std::size_t j = i + 1; j < es.size() && es[j].edge == es[i].edge && es[j].lo <= es[i].hi; j++) {
				const PrecheckEdgeSeg &s = es[i], &t = es[j];
				if (s.ring == t.ring) {
					const uint32_t lo = std::min(s.idx, t.idx), hi = std::max(s.idx, t.idx);
					if (hi - lo == 1 || (lo == 0 && hi == counts[s.ring] - 1)) continue; // adjacent
				}
				return true;
			}
	}
	return false;
}

} // namespace

bool multiPolygonDefinitelyValidClipped(const MultiPolygon &mp, const Box &box) {
	std::size_t points = 0;
	for (const Polygon &p : mp) {
		points += p.outer().size();
		for (const Ring &r : p.inners()) points += r.size();
	}
	const bool ok = !(points > kPrecheckBridgeMin && clipBridged(mp, box, precheckScratch)) && multiPolygonClear(mp, precheckScratch);
	precheckScratch.trim();
	return ok;
}

bool polygonDefinitelyValid(const Polygon &p) {
	const bool ok = polygonClear(p, precheckScratch);
	precheckScratch.trim();
	return ok;
}

bool multiPolygonDefinitelyValid(const MultiPolygon &mp) {
	const bool ok = multiPolygonClear(mp, precheckScratch);
	precheckScratch.trim();
	return ok;
}

int polygonDefinitelyValidOrReversed(const Polygon &p) {
	precheckOrient = 2;
	const bool ok = polygonClear(p, precheckScratch);
	const int o = precheckOrient;
	precheckOrient = 0;
	precheckScratch.trim();
	return ok ? o : 0;
}

int multiPolygonDefinitelyValidOrReversed(const MultiPolygon &mp) {
	precheckOrient = 2;
	const bool ok = multiPolygonClear(mp, precheckScratch);
	const int o = precheckOrient;
	precheckOrient = 0;
	precheckScratch.trim();
	return ok ? o : 0;
}

// Boost's cartesian_winding count of r around h (not 0: inside), or ok = false when it can't be
// told. Boost walks the segments whose x range holds h.x (a vertical ray): one with an end at
// h.x (math::equals) counts as touching the ray there, decided by y; one that spans h.x adds +-2
// when h is on the matching side of it, and makes h "on the boundary" when its side is 0 within
// Boost's zero band (which a very short segment far from h can have). Here an end near h.x, or a
// side that clearOrient can't call clearly (its band is far above Boost's), is unsure.
static int clearWinding(const Point &h, const Ring &r, double tol, bool &ok) {
	const double px = h.x();
	auto nearX = [px](double x) {
		return std::fabs(x - px) <= 4 * std::numeric_limits<double>::epsilon() * max3(1.0, std::fabs(x), std::fabs(px));
	};
	int w = 0;
	for (std::size_t i = 0; i + 1 < r.size(); i++) {
		const Point &a = r[i], &b = r[i + 1];
		if (nearX(a.x()) || nearX(b.x())) { ok = false; return 0; }
		if ((a.x() < px) == (b.x() < px)) continue;
		const int count = a.x() < px ? 2 : -2;
		const int side = clearOrient(a, b, h, tol);
		if (side == 0) { ok = false; return 0; }
		if (side * count > 0) w += count;
	}
	return w;
}

// Boost's covered_by(h, p) is certainly false: h is clearly outside p's outer ring, or clearly
// inside it and clearly inside one of its holes (point_in_geometry of a polygon)
static bool clearlyNotCovered(const Point &h, const Polygon &p, double tol) {
	bool ok = true;
	if (clearWinding(h, p.outer(), tol, ok) == 0) return ok;
	if (!ok) return false;
	for (const auto &inner : p.inners()) {
		const int w = clearWinding(h, inner, tol, ok);
		if (!ok) return false;
		if (w != 0) return true;
	}
	return false;
}

// Boost's intersects(a, b) is !disjoint: no turns between the rings of a and those of b
// (disjoint_linear; every pair of their segments is clearly apart, as the precheck decides it),
// and no ring of either has its first point covered by the other (rings_containing; that point
// is a vertex of a segment clear of the other's segments, so its location is clear too).
bool polygonsDefinitelyApart(const Polygon &a, const Polygon &b) {
	double maxAbs = 0;
	auto finite = [&](const Polygon &p) {
		// (a ring Boost doesn't walk, below 4 points, is left to it)
		if (p.outer().size() < 4 || !ringFinite(p.outer(), maxAbs)) return false;
		for (const auto &r : p.inners())
			if (r.size() < 4 || !ringFinite(r, maxAbs)) return false;
		return true;
	};
	if (!finite(a) || !finite(b)) return false;
	const double tol = kPrecheckEps * std::max(1.0, maxAbs);
	// (of all rings: a hole of an invalid polygon can lie outside its outer ring)
	auto envelope = [](const Polygon &poly) {
		PrecheckBox box;
		box.minX = box.minY = std::numeric_limits<double>::infinity();
		box.maxX = box.maxY = -std::numeric_limits<double>::infinity();
		auto add = [&](const Ring &r) {
			for (const Point &p : r) {
				box.minX = std::min(box.minX, p.x()); box.maxX = std::max(box.maxX, p.x());
				box.minY = std::min(box.minY, p.y()); box.maxY = std::max(box.maxY, p.y());
			}
		};
		add(poly.outer());
		for (const auto &r : poly.inners()) add(r);
		return box;
	};
	const PrecheckBox boxA = envelope(a), boxB = envelope(b);
	// The segments of p's rings whose boxes are not clearly apart from the other polygon's box
	// (the rest are clearly apart from all its segments); b's rings numbered apart from a's
	thread_local std::vector<PrecheckSeg> segsA, segsB;
	auto collect = [&](const Polygon &p, const PrecheckBox &other, uint32_t ringBase, std::vector<PrecheckSeg> &out) {
		out.clear();
		uint32_t ringIdx = ringBase;
		auto ring = [&](const Ring &r) {
			for (std::size_t i = 0; i + 1 < r.size(); i++) {
				const Point &p0 = r[i], &p1 = r[i + 1];
				PrecheckSeg s;
				s.minX = std::min(p0.x(), p1.x()); s.maxX = std::max(p0.x(), p1.x());
				s.minY = std::min(p0.y(), p1.y()); s.maxY = std::max(p0.y(), p1.y());
				if (s.maxX + tol < other.minX || other.maxX + tol < s.minX || s.maxY + tol < other.minY || other.maxY + tol < s.minY)
					continue;
				s.a = &p0;
				s.ring = ringIdx; s.idx = 0; // (rings of a and b are numbered apart: never adjacent)
				out.push_back(s);
			}
			ringIdx++;
		};
		ring(p.outer());
		for (const auto &r : p.inners()) ring(r);
	};
	if (a.inners().size() >= (1u << 30) || b.inners().size() >= (1u << 30)) return false;
	collect(a, boxB, 0, segsA);
	collect(b, boxA, 1u << 31, segsB);
	if (segsA.size() * segsB.size() > (std::size_t(1) << 16)) return false;
	for (const PrecheckSeg &s : segsA)
		for (const PrecheckSeg &t : segsB)
			if (!segsClear(s, t, tol)) return false;
	if (!clearlyNotCovered(b.outer().front(), a, tol)) return false;
	for (const auto &r : b.inners()) if (!clearlyNotCovered(r.front(), a, tol)) return false;
	if (!clearlyNotCovered(a.outer().front(), b, tol)) return false;
	for (const auto &r : a.inners()) if (!clearlyNotCovered(r.front(), b, tol)) return false;
	return true;
}

bool multiPolygonDefinitelyValidAfterClip(const MultiPolygon &mp) {
	precheckBoostSides = true;
	precheckUsedBoostSides = false;
	const bool ok = multiPolygonClear(mp, precheckScratch);
	precheckBoostSides = false;
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
// reversedValid: p is known to be valid once its rings are reversed.
//
// 0) Fast path: a ring without holes whose only fault is its orientation
// (e.g. after the y-flip into tile coordinates). On such a simple ring the
// dissolve finds no intersections and just reverses it, so do that directly.
// Done on p itself: returns true when the reversed p is the result, otherwise
// p is reversed back (unchanged) and repair_one_polygon_rest() goes on.
// The same for a polygon with holes that is valid once its rings are reversed
// (reversedValid): for those the dissolve below returns exactly that polygon, so
// the rings are reversed in place instead. Only with integer coordinates of
// |c| <= 2^15 and at most 2^20 points, where every computation on the way is
// exact (cross products, Boost's segment intersection ratios at shared
// endpoints, shoelace areas). Why the dissolve's result is p with every ring
// reversed (same points, same order, same first points, holes in order):
//  - geometry::correct(ring) of the outer ring reverses it (wrong orientation),
//    finds no self-intersection (the rings are clearly simple: only adjacent
//    segments meet, at their shared point, which Boost returns exactly, so the
//    scales are exactly 0 or 1 - or 0/0 for a repeated point - never both < 1)
//    and returns the reversed copy; each hole is already in outer orientation
//    and is returned as it is. All have area > 1E-12 (non-zero integers / 2).
//  - result_combine finds no intersecting pair (the rings are clearly apart),
//    so the holes stay in input order.
//  - difference(outer, holes) has no turns: Boost copies the outer ring and
//    each hole (clearly inside) reversed, in the order of their ring ids.
//  - acceptable(): the result is valid, its area is |area(p)|.
// (Checked on all recorded planet and local cases, and fuzzed.)
static bool repair_reversed_with_holes(Polygon &p, double minArea, double maxArea)
{
	double maxAbs = 0;
	std::size_t points = 0;
	auto exactRing = [&](const Ring &r) {
		points += r.size();
		for (const Point &q : r) {
			const double ax = std::fabs(q.x()), ay = std::fabs(q.y());
			if (!(ax <= 32768.0 && ay <= 32768.0)) return false;
			if (double(int32_t(ax)) != ax || double(int32_t(ay)) != ay) return false;
			maxAbs = std::max(maxAbs, std::max(ax, ay));
		}
		return true;
	};
	if (!exactRing(p.outer())) return false;
	for (const auto &r : p.inners()) if (!exactRing(r)) return false;
	if (points > (std::size_t(1) << 20)) return false;
	auto reverseAll = [&p]() {
		std::reverse(p.outer().begin(), p.outer().end());
		for (auto &r : p.inners()) std::reverse(r.begin(), r.end());
	};
	reverseAll();
	// the dissolve drops rings of area <= 1E-12 and checks the result's area
	bool ok = std::abs(geom::area(p.outer())) > 1E-12;
	for (const auto &r : p.inners()) ok = ok && std::abs(geom::area(r)) > 1E-12;
	if (ok) {
		const double a = std::abs(geom::area(p));
		ok = a >= minArea && a <= maxArea;
	}
	if (!ok) reverseAll();
	return ok;
}

static bool repair_one_polygon_reverse(Polygon &p, double minArea, double maxArea, bool reversedValid)
{
	if (!p.inners().empty()) return reversedValid && repair_reversed_with_holes(p, minArea, maxArea);
	std::reverse(p.outer().begin(), p.outer().end());
	const double revArea = std::abs(geom::area(p));
	bool revValid = false;
	if (revArea > 1E-12 && revArea >= minArea && revArea <= maxArea) {
		const bool pre = reversedValid || polygonDefinitelyValid(p);
		revValid = pre || geom::is_valid(p);
	}
	if (!revValid) std::reverse(p.outer().begin(), p.outer().end());
	return revValid;
}

// Steps 1) and 2) of the repair: appends the result to out and returns true on success.
static bool repair_one_polygon_rest(const Polygon &p, double minArea, double maxArea, MultiPolygon &out)
{
	auto acceptable = [&](const MultiPolygon &candidate) {
		if (!multiPolygonDefinitelyValid(candidate) && !geom::is_valid(candidate)) return false;
		const double a = std::abs(geom::area(candidate));
		return a >= minArea && a <= maxArea;
	};

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

bool repair_multi_polygon(MultiPolygon &mp, bool knownInvalid, bool *wholeValid, AreaBound areaBound)
{
	if (wholeValid) *wholeValid = false;
	// -1: mp is invalid (wrongly oriented), and valid with every ring reversed; so is each
	// polygon of it on its own
	int pre = 0;
	if (!knownInvalid) {
		pre = multiPolygonDefinitelyValidOrReversed(mp);
		if (pre > 0 || (pre == 0 && geom::is_valid(mp))) {
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
	//
	// The result is built in mp itself while every polygon's result is that
	// polygon (kept, or reversed in place): mp[0, i) is the result so far. From
	// the first polygon with another result on, it goes to out (mp[0, i) moved
	// there first), and replaces mp at the end.
	MultiPolygon out;
	bool inPlace = true;
	auto leavePlace = [&](std::size_t i) {
		if (!inPlace) return;
		inPlace = false;
		out.reserve(mp.size());
		for (std::size_t j = 0; j < i; j++) out.push_back(std::move(mp[j]));
	};
	bool allValid = true;
	const std::size_t count = mp.size();
	for (std::size_t i = 0; i < count; i++) {
		Polygon &p = mp[i];
		// With a single polygon, the whole-multipolygon check above already
		// established it is invalid.
		if (count > 1 && pre >= 0) {
			const bool prePolygon = polygonDefinitelyValid(p);
			if (prePolygon || geom::is_valid(p)) {
				if (!inPlace) out.push_back(std::move(p));
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
		if (repair_one_polygon_reverse(p, 0.5 * origArea, maxArea, pre < 0)) {
			if (!inPlace) out.push_back(std::move(p));
			continue;
		}
		leavePlace(i);
		if (!repair_one_polygon_rest(p, 0.5 * origArea, maxArea, out)) {
			out.push_back(std::move(p));
			allValid = false;
		}
	}

	if (!inPlace) mp = std::move(out);
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
	// Both end points beyond the same edge: the segment can't reach the box
	bool beyondOneEdge(const Point &a, const Point &b) const {
		return (a.x() < x0 && b.x() < x0) || (a.x() > x1 && b.x() > x1) ||
		       (a.y() < y0 && b.y() < y0) || (a.y() > y1 && b.y() > y1);
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
		// Most segments of a large polygon lie far from a small tile's box: reject
		// those with four comparisons before the clip computation
		const bool hit = !rc.beyondOneEdge(a, b) && rc.clipSegment(a, b, t0, t1, pa, pb) && t1 > t0;
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
	// A vertex can only equal p (Boost's equals: within a few machine epsilons) if its y is
	// this close to p's: the far ones skip that test, which made a scan of a long ring slow
	const double py = p.y();
	const double near = 1e-9 * std::max(1.0, std::fabs(py));
	for (std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
		const Point &a = ring[i], &b = ring[j];
		if (std::fabs(a.y() - py) <= near && geom::equals(a, p)) return 0;
		if ((a.y() > py) != (b.y() > py)) {
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

std::vector<Box> merge_nearby_boxes(std::vector<Box> boxes, double gap) {
	// A merged box can grow to reach one it passed over, so repeat until a
	// pass merges nothing.
	bool merged = true;
	while (merged) {
		merged = false;
		std::vector<Box> out;
		for (const Box &box : boxes) {
			const Box grown(
				Point(box.min_corner().x() - gap, box.min_corner().y() - gap),
				Point(box.max_corner().x() + gap, box.max_corner().y() + gap));
			auto near = std::find_if(out.begin(), out.end(),
				[&](const Box &other) { return geom::intersects(grown, other); });
			if (near == out.end()) {
				out.push_back(box);
			} else {
				geom::expand(*near, box);
				merged = true;
			}
		}
		boxes.swap(out);
	}
	return boxes;
}

std::vector<Box> multipolygon_index_boxes(const MultiPolygon &mp, double gap) {
	const Box envelope = geom::return_envelope<Box>(mp);
	if (mp.size() < 2) return { envelope };
	std::vector<Box> outers;
	double outersArea = 0;
	for (const Polygon &poly : mp) {
		outers.push_back(geom::return_envelope<Box>(poly.outer()));
		outersArea += geom::area(outers.back());
	}
	if (geom::area(envelope) <= 4 * outersArea) return { envelope };
	return merge_nearby_boxes(std::move(outers), gap);
}
