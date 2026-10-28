#pragma once

// Original source from https://github.com/mapbox/polylabel, licensed
// under ISC.
//
// Adapted to use Boost Geometry instead of MapBox's geometry library.
//
// Changes:
// - Default precision changed from 1 to 0.00001.
//   @mourner has some comments about what a reasonable precision value is
//   for latitude/longitude coordinates, see
//   https://github.com/mapbox/polylabel/issues/68#issuecomment-694906027
//   https://github.com/mapbox/polylabel/issues/103#issuecomment-1516623862
//
// Possible future changes:
// - Port the change described in https://github.com/mapbox/polylabel/issues/33,
//   implemented in Mapnik's Java renderer, to C++.
//   - But see counterexample: https://github.com/mapbox/polylabel/pull/63
//     @Fil also proposes an alternative approach there.
// - Pick precision as a function of the input geometry.

#include "geom.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <queue>
#include <vector>
#include <cstdint>
#include <limits>
// 2-wide SSE2 distance kernel. Only where the compiler cannot contract mul+add into FMA (that
// would round differently from the scalar original); elsewhere the original scalar loop is used.
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(__FMA__) && !defined(__FMA4__) && !defined(__AVX2__) && (!defined(__FLT_EVAL_METHOD__) || __FLT_EVAL_METHOD__ == 0)
#include <emmintrin.h>
#define POLYLABEL_SSE2 1
#endif

namespace mapbox {

namespace detail {

// get squared distance from a point to a segment
double getSegDistSq(const Point& p,
               const Point& a,
               const Point& b) {
    auto x = a.get<0>();
    auto y = a.get<1>();
    auto dx = b.get<0>() - x;
    auto dy = b.get<1>() - y;

    if (dx != 0 || dy != 0) {

        auto t = ((p.get<0>() - x) * dx + (p.get<1>() - y) * dy) / (dx * dx + dy * dy);

        if (t > 1) {
            x = b.get<0>();
            y = b.get<1>();

        } else if (t > 0) {
            x += dx * t;
            y += dy * t;
        }
    }

    dx = p.get<0>() - x;
    dy = p.get<1>() - y;

    return dx * dx + dy * dy;
}

// signed distance from point to polygon outline (negative if point is outside)
auto pointToPolygonDist(const Point& point, const Polygon& polygon) {
    bool inside = false;
    auto minDistSq = std::numeric_limits<double>::infinity();

    {
        const auto& ring = polygon.outer();
        for (std::size_t i = 0, len = ring.size(), j = len - 1; i < len; j = i++) {
            const auto& a = ring[i];
            const auto& b = ring[j];

            if ((a.get<1>() > point.get<1>()) != (b.get<1>() > point.get<1>()) &&
                (point.get<0>() < (b.get<0>() - a.get<0>()) * (point.get<1>() - a.get<1>()) / (b.get<1>() - a.get<1>()) + a.get<0>())) inside = !inside;

            minDistSq = std::min(minDistSq, getSegDistSq(point, a, b));
        }
    }

    for (const auto& ring : polygon.inners()) {
        for (std::size_t i = 0, len = ring.size(), j = len - 1; i < len; j = i++) {
            const auto& a = ring[i];
            const auto& b = ring[j];

            if ((a.get<1>() > point.get<1>()) != (b.get<1>() > point.get<1>()) &&
                (point.get<0>() < (b.get<0>() - a.get<0>()) * (point.get<1>() - a.get<1>()) / (b.get<1>() - a.get<1>()) + a.get<0>())) inside = !inside;

            minDistSq = std::min(minDistSq, getSegDistSq(point, a, b));
        }
    }

    return (inside ? 1 : -1) * std::sqrt(minDistSq);
}

// as pointToPolygonDist (bit-identical result), also reports the nearest edge:
// ring kr (-1 = outer, else inner index) and edge (ring[ki], ring[ki == 0 ? len - 1 : ki - 1])
double pointToPolygonDistKScalar(const Point& point, const Polygon& polygon, int& kr, std::size_t& ki) {
    bool inside = false;
    auto minDistSq = std::numeric_limits<double>::infinity();
    kr = -1;
    ki = 0;

    {
        const auto& ring = polygon.outer();
        for (std::size_t i = 0, len = ring.size(), j = len - 1; i < len; j = i++) {
            const auto& a = ring[i];
            const auto& b = ring[j];

            if ((a.get<1>() > point.get<1>()) != (b.get<1>() > point.get<1>()) &&
                (point.get<0>() < (b.get<0>() - a.get<0>()) * (point.get<1>() - a.get<1>()) / (b.get<1>() - a.get<1>()) + a.get<0>())) inside = !inside;

            // same selection as std::min(minDistSq, dd), including NaN handling
            const double dd = getSegDistSq(point, a, b);
            if (dd < minDistSq) { minDistSq = dd; ki = i; }
        }
    }

    int r = 0;
    for (const auto& ring : polygon.inners()) {
        for (std::size_t i = 0, len = ring.size(), j = len - 1; i < len; j = i++) {
            const auto& a = ring[i];
            const auto& b = ring[j];

            if ((a.get<1>() > point.get<1>()) != (b.get<1>() > point.get<1>()) &&
                (point.get<0>() < (b.get<0>() - a.get<0>()) * (point.get<1>() - a.get<1>()) / (b.get<1>() - a.get<1>()) + a.get<0>())) inside = !inside;

            const double dd = getSegDistSq(point, a, b);
            if (dd < minDistSq) { minDistSq = dd; kr = r; ki = i; }
        }
        r++;
    }

    return (inside ? 1 : -1) * std::sqrt(minDistSq);
}

#ifdef POLYLABEL_SSE2
// The edges of the polygon being labelled, one entry per (a = ring[i], b = ring[j]) exactly as
// pointToPolygonDistKScalar iterates them (outer ring first, then inners in order), in blocks of two.
// An odd count is padded with a NaN entry: its distance is NaN (never selected by the min) and its
// crossing test compares false, so it changes neither the distance nor the parity.
struct EdgePair {
    double ax[2], ay[2], bx[2], by[2], dx[2], dy[2], den[2];
};

struct PolygonEdges {
    const Polygon* polygon = nullptr; // polygon the arrays describe
    std::vector<EdgePair> pairs;
    std::vector<int> ring;            // per entry: -1 = outer, else inner index
    std::vector<std::size_t> edge;    // per entry: index i of a within its ring

    void build(const Polygon& p) {
        std::size_t n = p.outer().size();
        for (const auto& inner : p.inners()) n += inner.size();
        const double nan = std::numeric_limits<double>::quiet_NaN();
        pairs.assign((n + 1) / 2, EdgePair { {nan, nan}, {nan, nan}, {nan, nan}, {nan, nan}, {nan, nan}, {nan, nan}, {nan, nan} });
        ring.resize(n);
        edge.resize(n);
        std::size_t k = 0;
        auto add = [&](const Ring& rg, int r) {
            for (std::size_t i = 0, len = rg.size(), j = len - 1; i < len; j = i++, k++) {
                EdgePair& q = pairs[k >> 1];
                const std::size_t l = k & 1;
                q.ax[l] = rg[i].get<0>();
                q.ay[l] = rg[i].get<1>();
                q.bx[l] = rg[j].get<0>();
                q.by[l] = rg[j].get<1>();
                q.dx[l] = q.bx[l] - q.ax[l];
                q.dy[l] = q.by[l] - q.ay[l];
                q.den[l] = q.dx[l] * q.dx[l] + q.dy[l] * q.dy[l];
                ring[k] = r;
                edge[k] = i;
            }
        };
        add(p.outer(), -1);
        int r = 0;
        for (const auto& inner : p.inners()) add(inner, r++);
        polygon = &p;
    }

    // don't keep huge buffers alive after an exceptionally large polygon
    void release() {
        polygon = nullptr;
        if (pairs.capacity() > (1u << 12)) {
            std::vector<EdgePair>().swap(pairs);
            std::vector<int>().swap(ring);
            std::vector<std::size_t>().swap(edge);
        }
    }
};

// edges of the polygon currently inside polylabel() on this thread (nullptr otherwise)
static thread_local const PolygonEdges* activeEdges = nullptr;

// pointToPolygonDistKScalar over the edge arrays, two edges per step and without branches.
// Every lane evaluates the same IEEE operations in the same operand order as getSegDistSq and the
// crossing test: t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / (dx * dx + dy * dy), then
// x = t > 1 ? b.x : (t > 0 ? a.x + dx * t : a.x) (a degenerate edge gives t = NaN, so x = a.x as
// in the original's dx == dy == 0 branch). The minimum of the same set of values is exact in any
// order, MINPD keeps the accumulator for a NaN distance like 'dd < minDistSq', and the crossing
// parity is an XOR (order-independent).
inline double pointToPolygonDistKSoA(const Point& point, const PolygonEdges& e, int& kr, std::size_t& ki) {
    const __m128d px = _mm_set1_pd(point.get<0>());
    const __m128d py = _mm_set1_pd(point.get<1>());
    const __m128d one = _mm_set1_pd(1.0);
    const __m128d zero = _mm_setzero_pd();
    __m128d mn = _mm_set1_pd(std::numeric_limits<double>::infinity());
    __m128d par = _mm_setzero_pd();
    __m128i idx = _mm_set_epi64x(1, 0);
    __m128i best = _mm_setzero_si128();
    const __m128i two = _mm_set_epi64x(2, 2);

    const EdgePair* q = e.pairs.data();
    for (const EdgePair* end = q + e.pairs.size(); q != end; q++) {
        const __m128d ax = _mm_loadu_pd(q->ax);
        const __m128d ay = _mm_loadu_pd(q->ay);
        const __m128d bx = _mm_loadu_pd(q->bx);
        const __m128d by = _mm_loadu_pd(q->by);
        const __m128d dx = _mm_loadu_pd(q->dx);
        const __m128d dy = _mm_loadu_pd(q->dy);
        const __m128d pxa = _mm_sub_pd(px, ax);
        const __m128d pya = _mm_sub_pd(py, ay);

        // (a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x
        const __m128d straddle = _mm_xor_pd(_mm_cmpgt_pd(ay, py), _mm_cmpgt_pd(by, py));
        const __m128d xi = _mm_add_pd(_mm_div_pd(_mm_mul_pd(dx, pya), dy), ax);
        par = _mm_xor_pd(par, _mm_and_pd(straddle, _mm_cmplt_pd(px, xi)));

        // getSegDistSq
        const __m128d t = _mm_div_pd(_mm_add_pd(_mm_mul_pd(pxa, dx), _mm_mul_pd(pya, dy)), _mm_loadu_pd(q->den));
        const __m128d gt1 = _mm_cmpgt_pd(t, one);
        const __m128d gt0 = _mm_cmpgt_pd(t, zero);
        __m128d x = _mm_or_pd(_mm_and_pd(gt0, _mm_add_pd(ax, _mm_mul_pd(dx, t))), _mm_andnot_pd(gt0, ax));
        __m128d y = _mm_or_pd(_mm_and_pd(gt0, _mm_add_pd(ay, _mm_mul_pd(dy, t))), _mm_andnot_pd(gt0, ay));
        x = _mm_or_pd(_mm_and_pd(gt1, bx), _mm_andnot_pd(gt1, x));
        y = _mm_or_pd(_mm_and_pd(gt1, by), _mm_andnot_pd(gt1, y));
        const __m128d ex = _mm_sub_pd(px, x);
        const __m128d ey = _mm_sub_pd(py, y);
        const __m128d dd = _mm_add_pd(_mm_mul_pd(ex, ex), _mm_mul_pd(ey, ey));

        // per lane: if (dd < mn) { mn = dd; best = idx; }
        const __m128i lt = _mm_castpd_si128(_mm_cmplt_pd(dd, mn));
        mn = _mm_min_pd(dd, mn);
        best = _mm_or_si128(_mm_and_si128(lt, idx), _mm_andnot_si128(lt, best));
        idx = _mm_add_epi64(idx, two);
    }

    double m2[2];
    _mm_storeu_pd(m2, mn);
    std::int64_t b2[2];
    _mm_storeu_si128(reinterpret_cast<__m128i*>(b2), best);
    const int pm = _mm_movemask_pd(par);
    const bool inside = ((pm ^ (pm >> 1)) & 1) != 0;

    // both lanes are non-NaN (they start at +inf); either lane achieving the minimum is a nearest edge
    const int lane = m2[1] < m2[0] ? 1 : 0;
    const double minDistSq = m2[lane];
    if (e.ring.empty()) {
        kr = -1;
        ki = 0;
    } else {
        kr = e.ring[b2[lane]];
        ki = e.edge[b2[lane]];
    }

    return (inside ? 1 : -1) * std::sqrt(minDistSq);
}
#endif

// pointToPolygonDistKScalar, via the edge arrays when polylabel() has built them for this polygon
double pointToPolygonDistK(const Point& point, const Polygon& polygon, int& kr, std::size_t& ki) {
#ifdef POLYLABEL_SSE2
    const PolygonEdges* e = activeEdges;
    if (e && e->polygon == &polygon) {
        const double d = pointToPolygonDistKSoA(point, *e, kr, ki);
        return d;
    }
#endif
    return pointToPolygonDistKScalar(point, polygon, kr, ki);
}

struct Cell {
    Cell(const Point& c_, double h_, const Polygon& polygon)
        : c(c_),
          h(h_),
          d(pointToPolygonDistK(c, polygon, kr, ki)),
          max(d + h * std::sqrt(2))
        {}

    // nearest edge of the polygon outline to c (declared first: written while d is computed)
    std::size_t ki;
    int kr;
    Point c; // cell center
    double h; // half the cell size
    double d; // distance from cell center to polygon
    double max; // max distance to polygon within a cell
};

// get polygon centroid
Cell getCentroidCell(const Polygon& polygon) {
    double area = 0;
    double cx = 0, cy = 0;
    const auto& ring = polygon.outer();
    const Point& origin = ring.at(0);
    const double ox = origin.get<0>();
    const double oy = origin.get<1>();

    for (std::size_t i = 0, len = ring.size(), j = len - 1; i < len; j = i++) {
        const Point& a = ring[i];
        const Point& b = ring[j];
        const double ax = a.get<0>() - ox;
        const double ay = a.get<1>() - oy;
        const double bx = b.get<0>() - ox;
        const double by = b.get<1>() - oy;
        auto f = ax * by - bx * ay;
        cx += (ax + bx) * f;
        cy += (ay + by) * f;
        area += f * 3;
    }

    return Cell(area == 0 ? ring.at(0) : Point { ox + cx / area, oy + cy / area }, 0, polygon);
}

// Upper bound on pointToPolygonDist(p, polygon), from the edge nearest to `near.c`. The full scan
// computes min over all edges of getSegDistSq(p, a, b) with the same arguments, so its result is
// <= this edge's value; the margin covers FP contraction differences between call sites.
inline double edgeDistBound(const Point& p, const Cell& near, const Polygon& polygon) {
    const auto& ring = near.kr < 0 ? polygon.outer() : polygon.inners()[near.kr];
    const Point& a = ring[near.ki];
    const Point& b = ring[near.ki == 0 ? ring.size() - 1 : near.ki - 1];
    return std::sqrt(getSegDistSq(p, a, b)) * (1 + 1e-12) + 1e-11;
}


} // namespace detail

Point polylabel(const Polygon& polygon, double precision = 0.00001, bool debug = false) {
    using namespace detail;

    // find the bounding box of the outer ring
    Box envelope;
    geom::envelope(polygon.outer(), envelope);

    const Point size {
        envelope.max_corner().get<0>() - envelope.min_corner().get<0>(),
        envelope.max_corner().get<1>() - envelope.min_corner().get<1>()
    };

    const double cellSize = std::min(size.get<0>(), size.get<1>());
    double h = cellSize / 2;

    // a priority queue of cells in order of their "potential" (max distance to polygon)
    auto compareMax = [] (const Cell& a, const Cell& b) {
        if (a.max != b.max) return a.max < b.max;
        if (a.d != b.d) return a.d < b.d;
        if (a.h != b.h) return a.h > b.h;
        if (a.c.get<0>() != b.c.get<0>()) return a.c.get<0>() > b.c.get<0>();
        return a.c.get<1>() > b.c.get<1>();
    };

    if (cellSize == 0) {
        return envelope.min_corner();
    }

#ifdef POLYLABEL_SSE2
    // flatten the polygon's edges once for all distance evaluations of this call
    thread_local PolygonEdges edges;
    edges.build(polygon);
    activeEdges = &edges;
    struct EdgesGuard {
        ~EdgesGuard() { activeEdges = nullptr; edges.release(); }
    } edgesGuard;
#endif

    // take centroid as the first best guess
    // (computed before the initial grid so that grid cells can be pruned; Cell construction is pure)
    const Cell centroidCell = getCentroidCell(polygon);
    auto bestCell = centroidCell;

    // second guess: bounding box centroid
    Cell bboxCell(
        Point {
            envelope.min_corner().get<0>() + size.get<0>() / 2.0,
            envelope.min_corner().get<1>() + size.get<1>() / 2.0
        }, 0, polygon);
    if (bboxCell.d > bestCell.d) {
        bestCell = bboxCell;
    }

    // max-heap of cells, same algorithms as std::priority_queue (identical pop sequence);
    // reused per thread to avoid reallocation (polylabel does not recurse)
    thread_local std::vector<Cell> cellQueue;
    cellQueue.clear();
    auto pushCell = [&](const Point& p, double hc) {
        cellQueue.push_back(Cell(p, hc, polygon));
        std::push_heap(cellQueue.begin(), cellQueue.end(), compareMax);
    };

    // A cell whose distance is bounded by ub can neither replace bestCell (needs d > bestCell.d)
    // nor be subdivided when popped (cell.max - bestCell.d <= precision), as bestCell.d only grows.
    // Such a cell is a no-op; dropping it leaves the pop order of the others unchanged.
    const double sqrt2 = std::sqrt(2);
    auto provablyNoOp = [&](const Point& p, double ub, double hc) {
        if (!(ub <= bestCell.d && (ub + hc * sqrt2) - bestCell.d <= precision)) return false;
        (void)p;
        return true;
    };

    // cover polygon with initial cells

    std::size_t numProbes = 0;
    for (double x = envelope.min_corner().get<0>(); x < envelope.max_corner().get<0>(); x += cellSize) {
        for (double y = envelope.min_corner().get<1>(); y < envelope.max_corner().get<1>(); y += cellSize) {
            numProbes++;
            const Point p {x + h, y + h};
            const double ub = std::min(edgeDistBound(p, bboxCell, polygon), edgeDistBound(p, centroidCell, polygon));
            if (provablyNoOp(p, ub, h)) continue;
            pushCell(p, h);
        }
    }

    while (!cellQueue.empty()) {
        // pick the most promising cell from the queue
        std::pop_heap(cellQueue.begin(), cellQueue.end(), compareMax);
        auto cell = cellQueue.back();
        cellQueue.pop_back();

        // update the best cell if we found a better one
        if (cell.d > bestCell.d) {
            bestCell = cell;
            if (debug) std::cout << "found best " << ::round(1e4 * cell.d) / 1e4 << " after " << numProbes << " probes" << std::endl;
        }

        // do not drill down further if there's no chance of a better solution
        if (cell.max - bestCell.d <= precision) continue;

        // split the cell into four cells
        // (children whose distance is bounded by the parent's nearest edge may be provable no-ops)
        h = cell.h / 2;
        const Point children[4] = {
            {cell.c.get<0>() - h, cell.c.get<1>() - h},
            {cell.c.get<0>() + h, cell.c.get<1>() - h},
            {cell.c.get<0>() - h, cell.c.get<1>() + h},
            {cell.c.get<0>() + h, cell.c.get<1>() + h}
        };
        for (const Point& p : children) {
            if (provablyNoOp(p, edgeDistBound(p, cell, polygon), h)) continue;
            pushCell(p, h);
        }
        numProbes += 4;
    }

    if (debug) {
        std::cout << "num probes: " << numProbes << std::endl;
        std::cout << "best distance: " << bestCell.d << std::endl;
    }

    // don't keep a huge buffer alive after an exceptionally large polygon
    if (cellQueue.capacity() > (1u << 12)) std::vector<Cell>().swap(cellQueue);


    return bestCell.c;
}

} // namespace mapbox
