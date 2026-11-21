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
#include <memory>
// 2-wide SSE2 distance kernel. Only where the compiler cannot contract mul+add into FMA (that
// would round differently from the scalar original); elsewhere the original scalar loop is used.
// GCC/Clang only (the AVX2 kernels use target attributes and __builtin_cpu_supports).
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__)) && !defined(__FMA__) && !defined(__FMA4__) && !defined(__AVX2__) && (!defined(__FLT_EVAL_METHOD__) || __FLT_EVAL_METHOD__ == 0)
#include <emmintrin.h>
#include <immintrin.h>
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
// The entries are padded to a multiple of four with NaN entries: their distance is NaN (never
// selected by the min) and their crossing test compares false, so they change neither the
// distance nor the parity.
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
        pairs.assign(((n + 3) / 4) * 2, EdgePair { {nan, nan}, {nan, nan}, {nan, nan}, {nan, nan}, {nan, nan}, {nan, nan}, {nan, nan} });
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

// lanes 0, 1 from lo, 2, 3 from hi
__attribute__((target("avx2")))
static inline __m256d loadEdgeLanes(const double* lo, const double* hi) {
    return _mm256_insertf128_pd(_mm256_castpd128_pd256(_mm_loadu_pd(lo)), _mm_loadu_pd(hi), 1);
}

// pointToPolygonDistKSoA four edges at a time (two EdgePairs), on CPUs with AVX2. The same IEEE
// operations per lane (AVX2 without FMA: nothing can be contracted), the same minimum and parity.
__attribute__((target("avx2")))
inline double pointToPolygonDistKAvx2(const Point& point, const PolygonEdges& e, int& kr, std::size_t& ki) {
    const __m256d px = _mm256_set1_pd(point.get<0>());
    const __m256d py = _mm256_set1_pd(point.get<1>());
    const __m256d one = _mm256_set1_pd(1.0);
    const __m256d zero = _mm256_setzero_pd();
    __m256d mn = _mm256_set1_pd(std::numeric_limits<double>::infinity());
    __m256d par = _mm256_setzero_pd();
    __m256i idx = _mm256_set_epi64x(3, 2, 1, 0);
    __m256i best = _mm256_setzero_si256();
    const __m256i four = _mm256_set1_epi64x(4);
    // lanes 0, 1 from the first EdgePair, 2, 3 from the second
    const auto load = loadEdgeLanes;

    const EdgePair* q = e.pairs.data();
    for (const EdgePair* end = q + e.pairs.size(); q != end; q += 2) {
        const __m256d ax = load(q[0].ax, q[1].ax);
        const __m256d ay = load(q[0].ay, q[1].ay);
        const __m256d bx = load(q[0].bx, q[1].bx);
        const __m256d by = load(q[0].by, q[1].by);
        const __m256d dx = load(q[0].dx, q[1].dx);
        const __m256d dy = load(q[0].dy, q[1].dy);
        const __m256d pxa = _mm256_sub_pd(px, ax);
        const __m256d pya = _mm256_sub_pd(py, ay);

        const __m256d straddle = _mm256_xor_pd(_mm256_cmp_pd(ay, py, _CMP_GT_OQ), _mm256_cmp_pd(by, py, _CMP_GT_OQ));
        const __m256d xi = _mm256_add_pd(_mm256_div_pd(_mm256_mul_pd(dx, pya), dy), ax);
        par = _mm256_xor_pd(par, _mm256_and_pd(straddle, _mm256_cmp_pd(px, xi, _CMP_LT_OQ)));

        const __m256d t = _mm256_div_pd(_mm256_add_pd(_mm256_mul_pd(pxa, dx), _mm256_mul_pd(pya, dy)), load(q[0].den, q[1].den));
        const __m256d gt1 = _mm256_cmp_pd(t, one, _CMP_GT_OQ);
        const __m256d gt0 = _mm256_cmp_pd(t, zero, _CMP_GT_OQ);
        __m256d x = _mm256_or_pd(_mm256_and_pd(gt0, _mm256_add_pd(ax, _mm256_mul_pd(dx, t))), _mm256_andnot_pd(gt0, ax));
        __m256d y = _mm256_or_pd(_mm256_and_pd(gt0, _mm256_add_pd(ay, _mm256_mul_pd(dy, t))), _mm256_andnot_pd(gt0, ay));
        x = _mm256_or_pd(_mm256_and_pd(gt1, bx), _mm256_andnot_pd(gt1, x));
        y = _mm256_or_pd(_mm256_and_pd(gt1, by), _mm256_andnot_pd(gt1, y));
        const __m256d ex = _mm256_sub_pd(px, x);
        const __m256d ey = _mm256_sub_pd(py, y);
        const __m256d dd = _mm256_add_pd(_mm256_mul_pd(ex, ex), _mm256_mul_pd(ey, ey));

        const __m256i lt = _mm256_castpd_si256(_mm256_cmp_pd(dd, mn, _CMP_LT_OQ));
        mn = _mm256_min_pd(dd, mn);
        best = _mm256_blendv_epi8(best, idx, lt);
        idx = _mm256_add_epi64(idx, four);
    }

    double m4[4];
    _mm256_storeu_pd(m4, mn);
    std::int64_t b4[4];
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(b4), best);
    const int pm = _mm256_movemask_pd(par);
    const bool inside = (__builtin_popcount(pm) & 1) != 0;

    int lane = 0;
    for (int l = 1; l < 4; l++) if (m4[l] < m4[lane]) lane = l;
    const double minDistSq = m4[lane];
    if (e.ring.empty()) {
        kr = -1;
        ki = 0;
    } else {
        kr = e.ring[b4[lane]];
        ki = e.edge[b4[lane]];
    }

    return (inside ? 1 : -1) * std::sqrt(minDistSq);
}
#endif

// pointToPolygonDistKScalar, via the edge arrays when polylabel() has built them for this polygon
double pointToPolygonDistK(const Point& point, const Polygon& polygon, int& kr, std::size_t& ki) {
#ifdef POLYLABEL_SSE2
    const PolygonEdges* e = activeEdges;
    if (e && e->polygon == &polygon) {
        static const bool avx2 = __builtin_cpu_supports("avx2");
        if (avx2) return pointToPolygonDistKAvx2(point, *e, kr, ki);
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

// Upper bound on pointToPolygonDist(p, polygon), from the edge nearest to `nearCell.c`. The full scan
// computes min over all edges of getSegDistSq(p, a, b) with the same arguments, so its result is
// <= this edge's value; the margin covers FP contraction differences between call sites.
inline double edgeDistBound(const Point& p, const Cell& nearCell, const Polygon& polygon) {
    const auto& ring = nearCell.kr < 0 ? polygon.outer() : polygon.inners()[nearCell.kr];
    const Point& a = ring[nearCell.ki];
    const Point& b = ring[nearCell.ki == 0 ? ring.size() - 1 : nearCell.ki - 1];
    return std::sqrt(getSegDistSq(p, a, b)) * (1 + 1e-12) + 1e-11;
}


#ifdef POLYLABEL_SSE2
// ---------------------------------------------------------------------------------------------
// Fast path of polylabel() for polygons with finite coordinates of magnitude < 1e100 (all real
// input). It returns the bit-identical point:
//  - every distance is the same per-edge IEEE operation sequence as getSegDistSq and the crossing
//    test (see pointToPolygonDistKSoA), the minimum over the edges and the crossing parity are
//    order-independent, so a distance is the same double whichever kernel or lane computes it;
//  - cells are processed in the same order: the comparator is a strict total order on cells (no
//    NaN can occur with such coordinates), so the pop sequence is "always the largest cell present"
//    regardless of push order or heap implementation;
//  - only cells that are provably no-ops when popped (neither replace bestCell nor get split, as
//    bestCell.d only grows) are dropped; besides the edge bound of the original, a cell whose own
//    exact d and max already satisfy that is dropped instead of being pushed;
//  - once a popped cell is not split, no later cell is split either (later cells have max <= its
//    max, bestCell.d only grows), so the remaining pops only pick the first cell with the largest
//    d > bestCell.d in pop order: found by one scan instead of popping them all.
namespace fast {

// growable array of doubles that is not zero-filled (every entry used is written first)
struct RawBuffer {
    std::unique_ptr<double[]> p;
    std::size_t cap = 0;
    double* get(std::size_t n) {
        if (n > cap) { p.reset(new double[n]); cap = n; }
        return p.get();
    }
    void release(std::size_t limit) {
        if (cap > limit) { p.reset(); cap = 0; }
    }
};

struct Edges {
    RawBuffer buf;
    // SoA, npad entries each, 32-byte aligned; entries [n, npad) are NaN (never selected, never cross)
    double *ax = nullptr, *ay = nullptr, *bx = nullptr, *by = nullptr, *dx = nullptr, *dy = nullptr, *den = nullptr;
    std::size_t n = 0, npad = 0;
    // without blocks: edges [nn, n) are the zero-length ones (dx == dy == 0), moved behind the others
    // (the order of the edges changes neither the minimum nor the parity); for them the distance is
    // the distance to a (t = 0 / 0 = NaN takes neither branch), and they never straddle (ay == by)
    std::size_t nn = 0;

    // Blocks of kBlock consecutive edges with their bounding boxes (only when n >= kBlockMinEdges),
    // nbpad blocks (multiple of 4; padding blocks are empty boxes at +inf / -inf).
    static constexpr std::size_t kBlock = 8;
    static constexpr std::size_t kBlockMinEdges = 64;
    RawBuffer bbuf;
    double *xmin = nullptr, *xmaxm = nullptr, *ymin = nullptr, *ymax = nullptr, *xmax = nullptr;
    std::size_t nb = 0, nbpad = 0;
    double margin = 0; // absolute bound on rounding excursions of computed points beyond an edge's box

    // With more than kTreeMinBlocks blocks, a 4-ary tree of boxes above them: level 0 are the
    // blocks, node i of level l + 1 has the union box of nodes 4i .. 4i + 3 of level l (padding
    // nodes are empty boxes); the top level has at most four nodes. A node's tests (below) are
    // implied by those of each node under it, so skipping a node skips no block the flat scan
    // would evaluate under the same minimum.
    static constexpr std::size_t kTreeMinBlocks = 128;
    struct Level { const double *xmin, *xmaxm, *ymin, *ymax, *xmax; std::size_t count; };
    static constexpr int kMaxLevels = 16;
    Level lv[kMaxLevels];
    int nlv = 0;
    RawBuffer tbuf;

    // false if a coordinate is non-finite or |coordinate| >= 1e100 (then the reference path runs)
    bool build(const Polygon& p) {
        n = p.outer().size();
        for (const auto& inner : p.inners()) n += inner.size();
        const bool blocked = n >= kBlockMinEdges;
        npad = blocked ? (n + kBlock - 1) / kBlock * kBlock : (n + 3) & ~std::size_t(3);
        double* base = buf.get(7 * npad + 4);
        while (reinterpret_cast<std::uintptr_t>(base) & 31) base++;
        ax = base; ay = ax + npad; bx = ay + npad; by = bx + npad; dx = by + npad; dy = dx + npad; den = dy + npad;
        std::size_t k = 0;
        bool ok = true;
        auto add = [&](const Ring& rg) {
            for (std::size_t i = 0, len = rg.size(), j = len - 1; i < len; j = i++, k++) {
                const double x = rg[i].get<0>(), y = rg[i].get<1>();
                ok &= std::fabs(x) < 1e100 && std::fabs(y) < 1e100;
                ax[k] = x;
                ay[k] = y;
                bx[k] = rg[j].get<0>();
                by[k] = rg[j].get<1>();
                dx[k] = bx[k] - ax[k];
                dy[k] = by[k] - ay[k];
                den[k] = dx[k] * dx[k] + dy[k] * dy[k];
            }
        };
        add(p.outer());
        for (const auto& inner : p.inners()) add(inner);
        nn = n;
        if (!blocked) {
            double zx[kBlockMinEdges], zy[kBlockMinEdges];
            std::size_t w = 0, z = 0;
            for (std::size_t i = 0; i < n; i++) {
                if (dx[i] == 0 && dy[i] == 0) { zx[z] = ax[i]; zy[z] = ay[i]; z++; continue; }
                ax[w] = ax[i]; ay[w] = ay[i]; bx[w] = bx[i]; by[w] = by[i]; dx[w] = dx[i]; dy[w] = dy[i]; den[w] = den[i];
                w++;
            }
            nn = w;
            for (std::size_t i = 0; i < z; i++, w++) {
                ax[w] = bx[w] = zx[i]; ay[w] = by[w] = zy[i]; dx[w] = dy[w] = den[w] = 0;
            }
        }
        const double nan = std::numeric_limits<double>::quiet_NaN();
        for (; k < npad; k++) ax[k] = ay[k] = bx[k] = by[k] = dx[k] = dy[k] = den[k] = nan;
        nb = 0;
        if (!ok || !blocked) return ok;

        nb = npad / kBlock;
        nbpad = (nb + 3) & ~std::size_t(3);
        double* bb = bbuf.get(5 * nbpad + 4);
        while (reinterpret_cast<std::uintptr_t>(bb) & 31) bb++;
        xmin = bb; xmaxm = xmin + nbpad; ymin = xmaxm + nbpad; ymax = ymin + nbpad; xmax = ymax + nbpad;
        double maxAbs = 1;
        for (std::size_t b = 0; b < nb; b++) {
            double x0 = std::numeric_limits<double>::infinity(), x1 = -x0, y0 = x0, y1 = -x0;
            for (std::size_t i = b * kBlock, end = std::min(n, i + kBlock); i < end; i++) {
                x0 = std::min(x0, std::min(ax[i], bx[i]));
                x1 = std::max(x1, std::max(ax[i], bx[i]));
                y0 = std::min(y0, std::min(ay[i], by[i]));
                y1 = std::max(y1, std::max(ay[i], by[i]));
            }
            xmin[b] = x0; xmax[b] = x1; ymin[b] = y0; ymax[b] = y1;
            maxAbs = std::max(maxAbs, std::max(std::max(std::fabs(x0), std::fabs(x1)), std::max(std::fabs(y0), std::fabs(y1))));
        }
        // A computed nearest point or crossing abscissa of an edge lies within the edge's box up to
        // a few ulps of the largest coordinate magnitude (every coordinate and difference is below
        // 2 * maxAbs); 1e-12 * maxAbs is > 4000 such ulps.
        margin = 1e-12 * maxAbs;
        for (std::size_t b = 0; b < nb; b++) xmaxm[b] = xmax[b] + margin;
        const double inf = std::numeric_limits<double>::infinity();
        for (std::size_t b = nb; b < nbpad; b++) { xmin[b] = ymin[b] = inf; xmax[b] = xmaxm[b] = ymax[b] = -inf; }
        nlv = 0;
        if (nbpad > kTreeMinBlocks) {
            std::size_t total = 0;
            for (std::size_t c = nbpad; c > 4; ) { c = (c / 4 + 3) & ~std::size_t(3); total += c; }
            double* tb = tbuf.get(5 * total + 4);
            while (reinterpret_cast<std::uintptr_t>(tb) & 31) tb++;
            lv[0] = Level { xmin, xmaxm, ymin, ymax, xmax, nbpad };
            nlv = 1;
            while (lv[nlv - 1].count > 4) {
                const Level& c = lv[nlv - 1];
                const std::size_t cnt = c.count / 4, cpad = (cnt + 3) & ~std::size_t(3);
                double *x0 = tb, *xm = x0 + cpad, *y0 = xm + cpad, *y1 = y0 + cpad, *x1 = y1 + cpad;
                tb = x1 + cpad;
                for (std::size_t i = 0; i < cnt; i++) {
                    const std::size_t f = 4 * i;
                    x0[i] = std::min(std::min(c.xmin[f], c.xmin[f + 1]), std::min(c.xmin[f + 2], c.xmin[f + 3]));
                    xm[i] = std::max(std::max(c.xmaxm[f], c.xmaxm[f + 1]), std::max(c.xmaxm[f + 2], c.xmaxm[f + 3]));
                    y0[i] = std::min(std::min(c.ymin[f], c.ymin[f + 1]), std::min(c.ymin[f + 2], c.ymin[f + 3]));
                    y1[i] = std::max(std::max(c.ymax[f], c.ymax[f + 1]), std::max(c.ymax[f + 2], c.ymax[f + 3]));
                    x1[i] = std::max(std::max(c.xmax[f], c.xmax[f + 1]), std::max(c.xmax[f + 2], c.xmax[f + 3]));
                }
                for (std::size_t i = cnt; i < cpad; i++) { x0[i] = y0[i] = inf; x1[i] = xm[i] = y1[i] = -inf; }
                lv[nlv++] = Level { x0, xm, y0, y1, x1, cpad };
            }
        }
        return ok;
    }

    void release() {
        buf.release(1u << 15);
        bbuf.release(1u << 13);
        tbuf.release(1u << 13);
    }
};

struct FCell {
    Point c;    // cell center
    double h;   // half the cell size
    double d;   // distance from cell center to polygon
    double max; // max distance to polygon within a cell
    std::uint32_t k; // index of an edge nearest to c
};

inline bool lessCell(const FCell& a, const FCell& b) {
    if (a.max != b.max) return a.max < b.max;
    if (a.d != b.d) return a.d < b.d;
    if (a.h != b.h) return a.h > b.h;
    if (a.c.get<0>() != b.c.get<0>()) return a.c.get<0>() > b.c.get<0>();
    return a.c.get<1>() > b.c.get<1>();
}

// Max-heap of cells under lessCell. Any correct heap pops the same sequence (lessCell is a strict
// total order on distinct cells). Entries carry max, the first key, so most comparisons stay in the
// 16-byte entries; the cells themselves stay put in the pool.
struct HeapEntry {
    double max;
    std::uint32_t i;
};

struct Heap {
    // up to kLinear cells an unordered array with a linear scan per pop, then a binary heap
    static constexpr std::size_t kLinear = 32;
    FCell lin[kLinear + 1];
    double lm[kLinear + 1]; // lin[i].max
    std::size_t nl = 0;
    bool heap = false;
    std::vector<FCell> pool;
    std::vector<HeapEntry> h;

    void clear() { nl = 0; pool.clear(); h.clear(); heap = false; }
    bool empty() const { return heap ? h.empty() : nl == 0; }
    bool less(const HeapEntry& a, const HeapEntry& b) const {
        return a.max != b.max ? a.max < b.max : lessCell(pool[a.i], pool[b.i]);
    }
    void push(const FCell& c) {
        if (!heap) {
            lin[nl] = c;
            lm[nl] = c.max;
            if (++nl <= kLinear) return;
            for (std::size_t i = 0; i < nl; i++) {
                pool.push_back(lin[i]);
                h.push_back(HeapEntry { lin[i].max, static_cast<std::uint32_t>(i) });
            }
            std::make_heap(h.begin(), h.end(), [this](const HeapEntry& a, const HeapEntry& b) { return less(a, b); });
            heap = true;
            nl = 0;
            return;
        }
        const HeapEntry e { c.max, static_cast<std::uint32_t>(pool.size()) };
        pool.push_back(c);
        std::size_t i = h.size();
        h.push_back(e);
        while (i > 0) {
            const std::size_t p = (i - 1) / 2;
            if (!less(h[p], e)) break;
            h[i] = h[p];
            i = p;
        }
        h[i] = e;
    }
    FCell pop() {
        if (!heap) {
            // the largest max (branch-free), then the full comparison only if another cell has it too
            const std::size_t n = nl;
            std::size_t bi = 0;
            double bmax = lm[0];
            for (std::size_t i = 1; i < n; i++) {
                const double m = lm[i];
                const bool gt = m > bmax;
                bmax = gt ? m : bmax;
                bi = gt ? i : bi;
            }
            int ties = 0;
            for (std::size_t i = 0; i < n; i++) ties += lm[i] == bmax;
            if (ties > 1) {
                for (std::size_t i = 0; i < n; i++) if (lm[i] == bmax && lessCell(lin[bi], lin[i])) bi = i;
            }
            const FCell r = lin[bi];
            nl = n - 1;
            lin[bi] = lin[nl];
            lm[bi] = lm[nl];
            return r;
        }
        const std::uint32_t top = h[0].i;
        const HeapEntry e = h.back();
        h.pop_back();
        const std::size_t n = h.size();
        if (n) {
            std::size_t i = 0;
            for (;;) {
                std::size_t c = 2 * i + 1;
                if (c >= n) break;
                if (c + 1 < n && less(h[c], h[c + 1])) c++;
                if (!less(e, h[c])) break;
                h[i] = h[c];
                i = c;
            }
            h[i] = e;
        }
        return pool[top];
    }
    // the cells still queued, in no particular order
    template <class F> void forEach(F f) const {
        if (!heap) { for (std::size_t i = 0; i < nl; i++) f(lin[i]); return; }
        for (const HeapEntry& he : h) f(pool[he.i]);
    }
    void release() {
        if (pool.capacity() > (1u << 12)) { std::vector<FCell>().swap(pool); std::vector<HeapEntry>().swap(h); }
    }
};

// One point, edges in lanes (as pointToPolygonDistKSoA).
inline double distOneSse2(double pxs, double pys, const Edges& e, std::uint32_t& k) {
    const __m128d px = _mm_set1_pd(pxs);
    const __m128d py = _mm_set1_pd(pys);
    const __m128d one = _mm_set1_pd(1.0);
    const __m128d zero = _mm_setzero_pd();
    __m128d mn = _mm_set1_pd(std::numeric_limits<double>::infinity());
    __m128d par = _mm_setzero_pd();
    __m128i idx = _mm_set_epi64x(1, 0);
    __m128i best = _mm_setzero_si128();
    const __m128i two = _mm_set_epi64x(2, 2);
    for (std::size_t i = 0; i < e.npad; i += 2) {
        const __m128d ax = _mm_load_pd(e.ax + i);
        const __m128d ay = _mm_load_pd(e.ay + i);
        const __m128d bx = _mm_load_pd(e.bx + i);
        const __m128d by = _mm_load_pd(e.by + i);
        const __m128d dx = _mm_load_pd(e.dx + i);
        const __m128d dy = _mm_load_pd(e.dy + i);
        const __m128d pxa = _mm_sub_pd(px, ax);
        const __m128d pya = _mm_sub_pd(py, ay);
        const __m128d straddle = _mm_xor_pd(_mm_cmpgt_pd(ay, py), _mm_cmpgt_pd(by, py));
        const __m128d xi = _mm_add_pd(_mm_div_pd(_mm_mul_pd(dx, pya), dy), ax);
        par = _mm_xor_pd(par, _mm_and_pd(straddle, _mm_cmplt_pd(px, xi)));
        const __m128d t = _mm_div_pd(_mm_add_pd(_mm_mul_pd(pxa, dx), _mm_mul_pd(pya, dy)), _mm_load_pd(e.den + i));
        const __m128d gt1 = _mm_cmpgt_pd(t, one);
        const __m128d gt0 = _mm_cmpgt_pd(t, zero);
        __m128d x = _mm_or_pd(_mm_and_pd(gt0, _mm_add_pd(ax, _mm_mul_pd(dx, t))), _mm_andnot_pd(gt0, ax));
        __m128d y = _mm_or_pd(_mm_and_pd(gt0, _mm_add_pd(ay, _mm_mul_pd(dy, t))), _mm_andnot_pd(gt0, ay));
        x = _mm_or_pd(_mm_and_pd(gt1, bx), _mm_andnot_pd(gt1, x));
        y = _mm_or_pd(_mm_and_pd(gt1, by), _mm_andnot_pd(gt1, y));
        const __m128d ex = _mm_sub_pd(px, x);
        const __m128d ey = _mm_sub_pd(py, y);
        const __m128d dd = _mm_add_pd(_mm_mul_pd(ex, ex), _mm_mul_pd(ey, ey));
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
    const int lane = m2[1] < m2[0] ? 1 : 0;
    k = static_cast<std::uint32_t>(b2[lane]);
    return (inside ? 1 : -1) * std::sqrt(m2[lane]);
}

__attribute__((target("avx2")))
inline double distOneAvx2(double pxs, double pys, const Edges& e, std::uint32_t& k) {
    const __m256d px = _mm256_set1_pd(pxs);
    const __m256d py = _mm256_set1_pd(pys);
    const __m256d one = _mm256_set1_pd(1.0);
    const __m256d zero = _mm256_setzero_pd();
    __m256d mn = _mm256_set1_pd(std::numeric_limits<double>::infinity());
    __m256d par = _mm256_setzero_pd();
    __m256i idx = _mm256_set_epi64x(3, 2, 1, 0);
    __m256i best = _mm256_setzero_si256();
    const __m256i four = _mm256_set1_epi64x(4);
    for (std::size_t i = 0; i < e.npad; i += 4) {
        const __m256d ax = _mm256_load_pd(e.ax + i);
        const __m256d ay = _mm256_load_pd(e.ay + i);
        const __m256d bx = _mm256_load_pd(e.bx + i);
        const __m256d by = _mm256_load_pd(e.by + i);
        const __m256d dx = _mm256_load_pd(e.dx + i);
        const __m256d dy = _mm256_load_pd(e.dy + i);
        const __m256d pxa = _mm256_sub_pd(px, ax);
        const __m256d pya = _mm256_sub_pd(py, ay);
        const __m256d straddle = _mm256_xor_pd(_mm256_cmp_pd(ay, py, _CMP_GT_OQ), _mm256_cmp_pd(by, py, _CMP_GT_OQ));
        const __m256d xi = _mm256_add_pd(_mm256_div_pd(_mm256_mul_pd(dx, pya), dy), ax);
        par = _mm256_xor_pd(par, _mm256_and_pd(straddle, _mm256_cmp_pd(px, xi, _CMP_LT_OQ)));
        const __m256d t = _mm256_div_pd(_mm256_add_pd(_mm256_mul_pd(pxa, dx), _mm256_mul_pd(pya, dy)), _mm256_load_pd(e.den + i));
        const __m256d gt1 = _mm256_cmp_pd(t, one, _CMP_GT_OQ);
        const __m256d gt0 = _mm256_cmp_pd(t, zero, _CMP_GT_OQ);
        __m256d x = _mm256_blendv_pd(ax, _mm256_add_pd(ax, _mm256_mul_pd(dx, t)), gt0);
        __m256d y = _mm256_blendv_pd(ay, _mm256_add_pd(ay, _mm256_mul_pd(dy, t)), gt0);
        x = _mm256_blendv_pd(x, bx, gt1);
        y = _mm256_blendv_pd(y, by, gt1);
        const __m256d ex = _mm256_sub_pd(px, x);
        const __m256d ey = _mm256_sub_pd(py, y);
        const __m256d dd = _mm256_add_pd(_mm256_mul_pd(ex, ex), _mm256_mul_pd(ey, ey));
        const __m256i lt = _mm256_castpd_si256(_mm256_cmp_pd(dd, mn, _CMP_LT_OQ));
        mn = _mm256_min_pd(dd, mn);
        best = _mm256_blendv_epi8(best, idx, lt);
        idx = _mm256_add_epi64(idx, four);
    }
    double m4[4];
    _mm256_storeu_pd(m4, mn);
    std::int64_t b4[4];
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(b4), best);
    const int pm = _mm256_movemask_pd(par);
    const bool inside = (__builtin_popcount(pm) & 1) != 0;
    int lane = 0;
    for (int l = 1; l < 4; l++) if (m4[l] < m4[lane]) lane = l;
    k = static_cast<std::uint32_t>(b4[lane]);
    return (inside ? 1 : -1) * std::sqrt(m4[lane]);
}

// distOneAvx2 over the edges of the blocks that can matter. A block is skipped when
//  - none of its edges can flip the parity: py >= ymax or py < ymin (no edge straddles py), or
//    px > xmax + margin (every straddling edge's computed crossing abscissa xi <= xmax + margin,
//    so px < xi is false), and
//  - its edges cannot lower the minimum: a conservative lower bound on every computed squared
//    distance of its edges (from the gap between p and the box, less twice the margin, shrunk by
//    a relative 1e-12) exceeds the minimum so far.
// The skipped edges contribute nothing to the parity and only values above the final minimum, so
// the result is the same double; the block holding the hint edge (nearest to a nearby point) is
// evaluated first to get a small minimum early.
__attribute__((target("avx2")))
inline double distBlockedAvx2(double pxs, double pys, const Edges& e, std::uint32_t hint, std::uint32_t& k) {
    const __m256d px = _mm256_set1_pd(pxs);
    const __m256d py = _mm256_set1_pd(pys);
    const __m256d one = _mm256_set1_pd(1.0);
    const __m256d zero = _mm256_setzero_pd();
    __m256d mn = _mm256_set1_pd(std::numeric_limits<double>::infinity());
    __m256d par = _mm256_setzero_pd();
    __m256i best = _mm256_setzero_si256();

    auto evalBlock = [&](std::size_t b) __attribute__((target("avx2"), always_inline)) {
        for (std::size_t i = b * Edges::kBlock, end = i + Edges::kBlock; i < end; i += 4) {
            const __m256d ax = _mm256_load_pd(e.ax + i);
            const __m256d ay = _mm256_load_pd(e.ay + i);
            const __m256d bx = _mm256_load_pd(e.bx + i);
            const __m256d by = _mm256_load_pd(e.by + i);
            const __m256d dx = _mm256_load_pd(e.dx + i);
            const __m256d dy = _mm256_load_pd(e.dy + i);
            const __m256d pxa = _mm256_sub_pd(px, ax);
            const __m256d pya = _mm256_sub_pd(py, ay);
            const __m256d straddle = _mm256_xor_pd(_mm256_cmp_pd(ay, py, _CMP_GT_OQ), _mm256_cmp_pd(by, py, _CMP_GT_OQ));
            const __m256d xi = _mm256_add_pd(_mm256_div_pd(_mm256_mul_pd(dx, pya), dy), ax);
            par = _mm256_xor_pd(par, _mm256_and_pd(straddle, _mm256_cmp_pd(px, xi, _CMP_LT_OQ)));
            const __m256d t = _mm256_div_pd(_mm256_add_pd(_mm256_mul_pd(pxa, dx), _mm256_mul_pd(pya, dy)), _mm256_load_pd(e.den + i));
            const __m256d gt1 = _mm256_cmp_pd(t, one, _CMP_GT_OQ);
            const __m256d gt0 = _mm256_cmp_pd(t, zero, _CMP_GT_OQ);
            __m256d x = _mm256_blendv_pd(ax, _mm256_add_pd(ax, _mm256_mul_pd(dx, t)), gt0);
            __m256d y = _mm256_blendv_pd(ay, _mm256_add_pd(ay, _mm256_mul_pd(dy, t)), gt0);
            x = _mm256_blendv_pd(x, bx, gt1);
            y = _mm256_blendv_pd(y, by, gt1);
            const __m256d ex = _mm256_sub_pd(px, x);
            const __m256d ey = _mm256_sub_pd(py, y);
            const __m256d dd = _mm256_add_pd(_mm256_mul_pd(ex, ex), _mm256_mul_pd(ey, ey));
            const __m256i lt = _mm256_castpd_si256(_mm256_cmp_pd(dd, mn, _CMP_LT_OQ));
            mn = _mm256_min_pd(dd, mn);
            best = _mm256_blendv_epi8(best, _mm256_set_epi64x(i + 3, i + 2, i + 1, i), lt);
        }
    };
    auto hmin = [&]() __attribute__((target("avx2"), always_inline)) {
        const __m128d m2 = _mm_min_pd(_mm256_castpd256_pd128(mn), _mm256_extractf128_pd(mn, 1));
        return _mm_min_sd(m2, _mm_unpackhi_pd(m2, m2));
    };

    const std::size_t hb = hint / Edges::kBlock;
    evalBlock(hb);
    __m128d mnS = hmin();
    const __m256d m2 = _mm256_set1_pd(2 * e.margin);
    const __m256d shrink = _mm256_set1_pd(1 - 1e-12);
    // the nodes g .. g + 3 of a level that can hold a block to evaluate
    auto need = [&](const Edges::Level& L, std::size_t g) __attribute__((target("avx2"), always_inline)) {
        const __m256d x0 = _mm256_load_pd(L.xmin + g);
        const __m256d x1 = _mm256_load_pd(L.xmax + g);
        const __m256d y0 = _mm256_load_pd(L.ymin + g);
        const __m256d y1 = _mm256_load_pd(L.ymax + g);
        const __m256d needPar = _mm256_and_pd(_mm256_and_pd(_mm256_cmp_pd(py, y1, _CMP_LT_OQ), _mm256_cmp_pd(py, y0, _CMP_GE_OQ)),
                                              _mm256_cmp_pd(px, _mm256_load_pd(L.xmaxm + g), _CMP_LE_OQ));
        const __m256d gx = _mm256_max_pd(zero, _mm256_max_pd(_mm256_sub_pd(_mm256_sub_pd(x0, px), m2), _mm256_sub_pd(_mm256_sub_pd(px, x1), m2)));
        const __m256d gy = _mm256_max_pd(zero, _mm256_max_pd(_mm256_sub_pd(_mm256_sub_pd(y0, py), m2), _mm256_sub_pd(_mm256_sub_pd(py, y1), m2)));
        const __m256d lb = _mm256_mul_pd(_mm256_add_pd(_mm256_mul_pd(gx, gx), _mm256_mul_pd(gy, gy)), shrink);
        const __m256d needD = _mm256_cmp_pd(lb, _mm256_broadcastsd_pd(mnS), _CMP_LE_OQ);
        return _mm256_movemask_pd(_mm256_or_pd(needPar, needD));
    };
    if (e.nlv == 0) {
        const Edges::Level L0 { e.xmin, e.xmaxm, e.ymin, e.ymax, e.xmax, e.nbpad };
        for (std::size_t g = 0; g < e.nbpad; g += 4) {
            int mask = need(L0, g);
            while (mask) {
                const std::size_t b = g + __builtin_ctz(mask);
                mask &= mask - 1;
                if (b == hb) continue;
                evalBlock(b);
                mnS = hmin();
            }
        }
    } else {
        // depth first, children in index order
        struct Item { int level; std::size_t g; };
        Item st[4 * Edges::kMaxLevels];
        int sp = 0;
        st[sp++] = Item { e.nlv - 1, 0 };
        while (sp) {
            const Item it = st[--sp];
            int mask = need(e.lv[it.level], it.g);
            if (it.level == 0) {
                while (mask) {
                    const std::size_t b = it.g + __builtin_ctz(mask);
                    mask &= mask - 1;
                    if (b == hb) continue;
                    evalBlock(b);
                    mnS = hmin();
                }
            } else {
                while (mask) {
                    const int c = 31 - __builtin_clz(mask);
                    mask &= ~(1 << c);
                    st[sp++] = Item { it.level - 1, 4 * (it.g + c) };
                }
            }
        }
    }

    double m4[4];
    _mm256_storeu_pd(m4, mn);
    std::int64_t b4[4];
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(b4), best);
    const int pm = _mm256_movemask_pd(par);
    const bool inside = (__builtin_popcount(pm) & 1) != 0;
    int lane = 0;
    for (int l = 1; l < 4; l++) if (m4[l] < m4[lane]) lane = l;
    k = static_cast<std::uint32_t>(b4[lane]);
    return (inside ? 1 : -1) * std::sqrt(m4[lane]);
}

// Two points in lanes, edges in sequence (the same per-edge operations as above per lane).
inline void distTwoSse2(const double* pxs, const double* pys, const Edges& e, double* d, std::uint32_t* k) {
    const __m128d px = _mm_loadu_pd(pxs);
    const __m128d py = _mm_loadu_pd(pys);
    const __m128d one = _mm_set1_pd(1.0);
    const __m128d zero = _mm_setzero_pd();
    __m128d mn = _mm_set1_pd(std::numeric_limits<double>::infinity());
    __m128d par = _mm_setzero_pd();
    __m128i best = _mm_setzero_si128();
    for (std::size_t i = 0; i < e.n; i++) {
        const __m128d ax = _mm_set1_pd(e.ax[i]);
        const __m128d ay = _mm_set1_pd(e.ay[i]);
        const __m128d bx = _mm_set1_pd(e.bx[i]);
        const __m128d by = _mm_set1_pd(e.by[i]);
        const __m128d dx = _mm_set1_pd(e.dx[i]);
        const __m128d dy = _mm_set1_pd(e.dy[i]);
        const __m128d pxa = _mm_sub_pd(px, ax);
        const __m128d pya = _mm_sub_pd(py, ay);
        const __m128d straddle = _mm_xor_pd(_mm_cmpgt_pd(ay, py), _mm_cmpgt_pd(by, py));
        const __m128d xi = _mm_add_pd(_mm_div_pd(_mm_mul_pd(dx, pya), dy), ax);
        par = _mm_xor_pd(par, _mm_and_pd(straddle, _mm_cmplt_pd(px, xi)));
        const __m128d t = _mm_div_pd(_mm_add_pd(_mm_mul_pd(pxa, dx), _mm_mul_pd(pya, dy)), _mm_set1_pd(e.den[i]));
        const __m128d gt1 = _mm_cmpgt_pd(t, one);
        const __m128d gt0 = _mm_cmpgt_pd(t, zero);
        __m128d x = _mm_or_pd(_mm_and_pd(gt0, _mm_add_pd(ax, _mm_mul_pd(dx, t))), _mm_andnot_pd(gt0, ax));
        __m128d y = _mm_or_pd(_mm_and_pd(gt0, _mm_add_pd(ay, _mm_mul_pd(dy, t))), _mm_andnot_pd(gt0, ay));
        x = _mm_or_pd(_mm_and_pd(gt1, bx), _mm_andnot_pd(gt1, x));
        y = _mm_or_pd(_mm_and_pd(gt1, by), _mm_andnot_pd(gt1, y));
        const __m128d ex = _mm_sub_pd(px, x);
        const __m128d ey = _mm_sub_pd(py, y);
        const __m128d dd = _mm_add_pd(_mm_mul_pd(ex, ex), _mm_mul_pd(ey, ey));
        const __m128i lt = _mm_castpd_si128(_mm_cmplt_pd(dd, mn));
        mn = _mm_min_pd(dd, mn);
        const __m128i idx = _mm_set1_epi64x(static_cast<long long>(i));
        best = _mm_or_si128(_mm_and_si128(lt, idx), _mm_andnot_si128(lt, best));
    }
    double m2[2];
    _mm_storeu_pd(m2, mn);
    std::int64_t b2[2];
    _mm_storeu_si128(reinterpret_cast<__m128i*>(b2), best);
    const int pm = _mm_movemask_pd(par);
    for (int l = 0; l < 2; l++) {
        const bool inside = ((pm >> l) & 1) != 0;
        d[l] = (inside ? 1 : -1) * std::sqrt(m2[l]);
        k[l] = static_cast<std::uint32_t>(b2[l]);
    }
}

// Four points in lanes, edges in sequence. Also max = d + hs per lane, and returns the lanes that
// are not no-ops (as the noOp test). TrackK: report a nearest edge per lane in k (else k is not
// written).
template <bool TrackK>
__attribute__((target("avx2")))
inline int distFourAvx2T(const double* pxs, const double* pys, const Edges& e, double* d, std::uint32_t* k,
                         double hs, double bestD, double precision, double* mx) {
    const __m256d px = _mm256_loadu_pd(pxs);
    const __m256d py = _mm256_loadu_pd(pys);
    const __m256d one = _mm256_set1_pd(1.0);
    const __m256d zero = _mm256_setzero_pd();
    __m256d mn = _mm256_set1_pd(std::numeric_limits<double>::infinity());
    __m256d par = _mm256_setzero_pd();
    __m256i best = _mm256_setzero_si256();
    for (std::size_t i = 0; i < e.nn; i++) {
        const __m256d ax = _mm256_broadcast_sd(e.ax + i);
        const __m256d ay = _mm256_broadcast_sd(e.ay + i);
        const __m256d bx = _mm256_broadcast_sd(e.bx + i);
        const __m256d by = _mm256_broadcast_sd(e.by + i);
        const __m256d dx = _mm256_broadcast_sd(e.dx + i);
        const __m256d dy = _mm256_broadcast_sd(e.dy + i);
        const __m256d pxa = _mm256_sub_pd(px, ax);
        const __m256d pya = _mm256_sub_pd(py, ay);
        const __m256d straddle = _mm256_xor_pd(_mm256_cmp_pd(ay, py, _CMP_GT_OQ), _mm256_cmp_pd(by, py, _CMP_GT_OQ));
        const __m256d xi = _mm256_add_pd(_mm256_div_pd(_mm256_mul_pd(dx, pya), dy), ax);
        par = _mm256_xor_pd(par, _mm256_and_pd(straddle, _mm256_cmp_pd(px, xi, _CMP_LT_OQ)));
        const __m256d t = _mm256_div_pd(_mm256_add_pd(_mm256_mul_pd(pxa, dx), _mm256_mul_pd(pya, dy)), _mm256_broadcast_sd(e.den + i));
        const __m256d gt1 = _mm256_cmp_pd(t, one, _CMP_GT_OQ);
        const __m256d gt0 = _mm256_cmp_pd(t, zero, _CMP_GT_OQ);
        __m256d x = _mm256_blendv_pd(ax, _mm256_add_pd(ax, _mm256_mul_pd(dx, t)), gt0);
        __m256d y = _mm256_blendv_pd(ay, _mm256_add_pd(ay, _mm256_mul_pd(dy, t)), gt0);
        x = _mm256_blendv_pd(x, bx, gt1);
        y = _mm256_blendv_pd(y, by, gt1);
        const __m256d ex = _mm256_sub_pd(px, x);
        const __m256d ey = _mm256_sub_pd(py, y);
        const __m256d dd = _mm256_add_pd(_mm256_mul_pd(ex, ex), _mm256_mul_pd(ey, ey));
        if (TrackK) {
            const __m256i lt = _mm256_castpd_si256(_mm256_cmp_pd(dd, mn, _CMP_LT_OQ));
            best = _mm256_blendv_epi8(best, _mm256_set1_epi64x(static_cast<long long>(i)), lt);
        }
        mn = _mm256_min_pd(dd, mn);
    }
    for (std::size_t i = e.nn; i < e.n; i++) {
        const __m256d ex = _mm256_sub_pd(px, _mm256_broadcast_sd(e.ax + i));
        const __m256d ey = _mm256_sub_pd(py, _mm256_broadcast_sd(e.ay + i));
        const __m256d dd = _mm256_add_pd(_mm256_mul_pd(ex, ex), _mm256_mul_pd(ey, ey));
        if (TrackK) {
            const __m256i lt = _mm256_castpd_si256(_mm256_cmp_pd(dd, mn, _CMP_LT_OQ));
            best = _mm256_blendv_epi8(best, _mm256_set1_epi64x(static_cast<long long>(i)), lt);
        }
        mn = _mm256_min_pd(dd, mn);
    }
    // (inside ? 1 : -1) * std::sqrt(minDistSq): VSQRTPD rounds as SQRTSD, and multiplying the
    // non-NaN root by -1 is exactly a sign flip
    const __m256d neg = _mm256_andnot_pd(par, _mm256_set1_pd(-0.0));
    const __m256d dv = _mm256_xor_pd(_mm256_sqrt_pd(mn), neg);
    const __m256d mv = _mm256_add_pd(dv, _mm256_set1_pd(hs));
    _mm256_storeu_pd(d, dv);
    _mm256_storeu_pd(mx, mv);
    if (TrackK) {
        std::int64_t b4[4];
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(b4), best);
        for (int l = 0; l < 4; l++) k[l] = static_cast<std::uint32_t>(b4[l]);
    }
    const __m256d bv = _mm256_set1_pd(bestD);
    const __m256d noop = _mm256_and_pd(_mm256_cmp_pd(dv, bv, _CMP_LE_OQ), _mm256_cmp_pd(_mm256_sub_pd(mv, bv), _mm256_set1_pd(precision), _CMP_LE_OQ));
    return ~_mm256_movemask_pd(noop) & 15;
}

__attribute__((target("avx2")))
inline int distFourAvx2(const double* pxs, const double* pys, const Edges& e, double* d, std::uint32_t* k,
                        double hs, double bestD, double precision, double* mx) {
    return distFourAvx2T<true>(pxs, pys, e, d, k, hs, bestD, precision, mx);
}

inline bool useAvx2() {
#ifdef POLYLABEL_TEST_NO_AVX2
    return false;
#else
    static const bool avx2 = __builtin_cpu_supports("avx2");
    return avx2;
#endif
}

// distances of m <= 4 points (lanes m..3 of the arrays are scratch)
inline void distMany(double* pxs, double* pys, int m, const Edges& e, double* d, std::uint32_t* k) {
    if (m == 1) { d[0] = useAvx2() ? distOneAvx2(pxs[0], pys[0], e, k[0]) : distOneSse2(pxs[0], pys[0], e, k[0]); return; }
    for (int l = m; l < 4; l++) { pxs[l] = pxs[0]; pys[l] = pys[0]; }
    if (useAvx2()) { double mx[4]; distFourAvx2(pxs, pys, e, d, k, 0, 0, 0, mx); return; }
    distTwoSse2(pxs, pys, e, d, k);
    if (m > 2) distTwoSse2(pxs + 2, pys + 2, e, d + 2, k + 2);
}

// Upper bound on the distance at p from the edge k (as edgeDistBound).
inline double edgeBound(const Point& p, const Edges& e, std::uint32_t k) {
    return std::sqrt(getSegDistSq(p, Point { e.ax[k], e.ay[k] }, Point { e.bx[k], e.by[k] })) * (1 + 1e-12) + 1e-11;
}

// polygons with fewer edges evaluate every candidate cell (no edge bound first)
constexpr std::size_t kBoundMinEdges = Edges::kBlockMinEdges;

// Signed distances of four points (lanes) to the polygon: distFourAvx2T without the nearest edge,
// in registers. The nearest point of the segment is computed as
// x = t > 1 ? b.x : a.x + dx * max(t, 0) (likewise y): for t > 0 that is getSegDistSq's
// a.x + dx * t; for t <= 0 or NaN (max returns its second operand 0 for NaN) it is a.x + dx * 0,
// which equals a.x unless a.x is a zero, where only the sign of the zero can differ; p.x - x then
// differs at most in the sign of a zero, and the squares and the distance are the same.
__attribute__((target("avx2"), always_inline))
inline __m256d distFourSmall(const __m256d px, const __m256d py, const Edges& e) {
    const __m256d one = _mm256_set1_pd(1.0);
    const __m256d zero = _mm256_setzero_pd();
    __m256d mn = _mm256_set1_pd(std::numeric_limits<double>::infinity());
    __m256d par = _mm256_setzero_pd();
    for (std::size_t i = 0; i < e.nn; i++) {
        const __m256d ax = _mm256_broadcast_sd(e.ax + i);
        const __m256d ay = _mm256_broadcast_sd(e.ay + i);
        const __m256d bx = _mm256_broadcast_sd(e.bx + i);
        const __m256d by = _mm256_broadcast_sd(e.by + i);
        const __m256d dx = _mm256_broadcast_sd(e.dx + i);
        const __m256d dy = _mm256_broadcast_sd(e.dy + i);
        const __m256d pxa = _mm256_sub_pd(px, ax);
        const __m256d pya = _mm256_sub_pd(py, ay);
        const __m256d straddle = _mm256_xor_pd(_mm256_cmp_pd(ay, py, _CMP_GT_OQ), _mm256_cmp_pd(by, py, _CMP_GT_OQ));
        const __m256d xi = _mm256_add_pd(_mm256_div_pd(_mm256_mul_pd(dx, pya), dy), ax);
        par = _mm256_xor_pd(par, _mm256_and_pd(straddle, _mm256_cmp_pd(px, xi, _CMP_LT_OQ)));
        const __m256d t = _mm256_div_pd(_mm256_add_pd(_mm256_mul_pd(pxa, dx), _mm256_mul_pd(pya, dy)), _mm256_broadcast_sd(e.den + i));
        const __m256d gt1 = _mm256_cmp_pd(t, one, _CMP_GT_OQ);
        const __m256d tc = _mm256_max_pd(t, zero);
        const __m256d x = _mm256_blendv_pd(_mm256_add_pd(ax, _mm256_mul_pd(dx, tc)), bx, gt1);
        const __m256d y = _mm256_blendv_pd(_mm256_add_pd(ay, _mm256_mul_pd(dy, tc)), by, gt1);
        const __m256d ex = _mm256_sub_pd(px, x);
        const __m256d ey = _mm256_sub_pd(py, y);
        mn = _mm256_min_pd(_mm256_add_pd(_mm256_mul_pd(ex, ex), _mm256_mul_pd(ey, ey)), mn);
    }
    for (std::size_t i = e.nn; i < e.n; i++) {
        const __m256d ex = _mm256_sub_pd(px, _mm256_broadcast_sd(e.ax + i));
        const __m256d ey = _mm256_sub_pd(py, _mm256_broadcast_sd(e.ay + i));
        mn = _mm256_min_pd(_mm256_add_pd(_mm256_mul_pd(ex, ex), _mm256_mul_pd(ey, ey)), mn);
    }
    return _mm256_xor_pd(_mm256_sqrt_pd(mn), _mm256_andnot_pd(par, _mm256_set1_pd(-0.0)));
}

// Small polygons on AVX2 CPUs (fewer than kBoundMinEdges edges: every candidate cell is evaluated,
// four per kernel call): the decisions of polylabelFast with the queued cells in small arrays.
// Returns false, having decided nothing, if more than kSmallQueue cells would be queued (the
// caller then runs polylabelFast's general loop from the start; evaluation is pure).
constexpr int kSmallQueue = 64;

__attribute__((target("avx2")))
inline bool polylabelSmallAvx2(double precision, const Box& envelope, double cellSize, const Edges& e,
                               const Point& centroid, const Point& bboxCenter, Point& out) {
    alignas(32) double qx[kSmallQueue], qy[kSmallQueue], qh[kSmallQueue], qd[kSmallQueue], qm[kSmallQueue];
    int qn = 0;
    // lessCell on queue entries
    auto less = [&](int a, int b) {
        if (qm[a] != qm[b]) return qm[a] < qm[b];
        if (qd[a] != qd[b]) return qd[a] < qd[b];
        if (qh[a] != qh[b]) return qh[a] > qh[b];
        if (qx[a] != qx[b]) return qx[a] > qx[b];
        return qy[a] > qy[b];
    };
    double bestX = 0, bestY = 0, bestD = 0;

    // centroid, bbox center and the initial grid, four per kernel call (lanes 0, 1 of the first
    // call are the two first guesses); the grid decisions only depend on bestD after the guesses
    const double h0 = cellSize / 2;
    const double hs0 = h0 * std::sqrt(2);
    alignas(32) double gx[4], gy[4], gd[4];
    gx[0] = centroid.get<0>(); gy[0] = centroid.get<1>();
    gx[1] = bboxCenter.get<0>(); gy[1] = bboxCenter.get<1>();
    int gm = 2;
    bool first = true;
    auto flushGrid = [&]() __attribute__((target("avx2"), always_inline)) {
        for (int l = gm; l < 4; l++) { gx[l] = gx[0]; gy[l] = gy[0]; }
        _mm256_store_pd(gd, distFourSmall(_mm256_load_pd(gx), _mm256_load_pd(gy), e));
        int l = 0;
        if (first) {
            // bestCell = centroidCell; if (bboxCell.d > bestCell.d) bestCell = bboxCell
            bestX = gx[0]; bestY = gy[0]; bestD = gd[0];
            if (gd[1] > bestD) { bestX = gx[1]; bestY = gy[1]; bestD = gd[1]; }
            first = false;
            l = 2;
        }
        for (; l < gm; l++) {
            const double d = gd[l], m = d + hs0;
            if (d <= bestD && m - bestD <= precision) continue;
            if (qn == kSmallQueue) return false;
            qx[qn] = gx[l]; qy[qn] = gy[l]; qh[qn] = h0; qd[qn] = d; qm[qn] = m; qn++;
        }
        gm = 0;
        return true;
    };
    for (double x = envelope.min_corner().get<0>(); x < envelope.max_corner().get<0>(); x += cellSize) {
        for (double y = envelope.min_corner().get<1>(); y < envelope.max_corner().get<1>(); y += cellSize) {
            gx[gm] = x + h0; gy[gm] = y + h0;
            if (++gm == 4 && !flushGrid()) return false;
        }
    }
    if ((gm || first) && !flushGrid()) return false;

    // children of a cell centred at c with half size 2h: c + (-h, -h), (h, -h), (-h, h), (h, h)
    const __m256d sxv = _mm256_set_pd(1, -1, 1, -1);
    const __m256d syv = _mm256_set_pd(1, 1, -1, -1);
    const __m256d sqrt2v = _mm256_set1_pd(std::sqrt(2));
    while (qn) {
        // pop the largest cell under lessCell: largest max, the full comparison only for ties
        int bi = 0;
        double bmax = qm[0];
        for (int i = 1; i < qn; i++) {
            const bool gt = qm[i] > bmax;
            bmax = gt ? qm[i] : bmax;
            bi = gt ? i : bi;
        }
        int ties = 0;
        for (int i = 0; i < qn; i++) ties += qm[i] == bmax;
        if (ties > 1) {
            for (int i = 0; i < qn; i++) if (qm[i] == bmax && less(bi, i)) bi = i;
        }
        const double cx = qx[bi], cy = qy[bi], ch = qh[bi], cd = qd[bi], cm = qm[bi];
        qn--;
        qx[bi] = qx[qn]; qy[bi] = qy[qn]; qh[bi] = qh[qn]; qd[bi] = qd[qn]; qm[bi] = qm[qn];

        if (cd > bestD) { bestX = cx; bestY = cy; bestD = cd; }

        if (cm - bestD <= precision) {
            // no remaining cell is split: the first cell in pop order with the largest d > bestD
            int cand = -1;
            for (int i = 0; i < qn; i++) {
                if (qd[i] > bestD && (cand < 0 || qd[i] > qd[cand] || (qd[i] == qd[cand] && less(cand, i)))) cand = i;
            }
            if (cand >= 0) { bestX = qx[cand]; bestY = qy[cand]; }
            break;
        }

        const double h = ch / 2;
        const __m256d hv = _mm256_set1_pd(h);
        const __m256d px = _mm256_add_pd(_mm256_set1_pd(cx), _mm256_mul_pd(hv, sxv));
        const __m256d py = _mm256_add_pd(_mm256_set1_pd(cy), _mm256_mul_pd(hv, syv));
        const __m256d dv = distFourSmall(px, py, e);
        const __m256d mv = _mm256_add_pd(dv, _mm256_mul_pd(hv, sqrt2v));
        const __m256d bv = _mm256_set1_pd(bestD);
        const __m256d noop = _mm256_and_pd(_mm256_cmp_pd(dv, bv, _CMP_LE_OQ), _mm256_cmp_pd(_mm256_sub_pd(mv, bv), _mm256_set1_pd(precision), _CMP_LE_OQ));
        int keep = ~_mm256_movemask_pd(noop) & 15;
        if (!keep) continue;
        if (qn + __builtin_popcount(keep) > kSmallQueue) return false;
        alignas(32) double ax[4], ay[4], ad[4], am[4];
        _mm256_store_pd(ax, px); _mm256_store_pd(ay, py); _mm256_store_pd(ad, dv); _mm256_store_pd(am, mv);
        do {
            const int l = __builtin_ctz(keep);
            keep &= keep - 1;
            qx[qn] = ax[l]; qy[qn] = ay[l]; qh[qn] = h; qd[qn] = ad[l]; qm[qn] = am[l]; qn++;
        } while (keep);
    }
    out = Point { bestX, bestY };
    return true;
}

inline Point polylabelFast(const Polygon& polygon, double precision, const Box& envelope, const Point& size, double cellSize, Edges& e) {
    const double sqrt2 = std::sqrt(2);
    const bool bound = e.n >= kBoundMinEdges;
    const bool blocked = e.nb > 0 && useAvx2();

    auto makeCell = [&](const Point& c, double h, double d, std::uint32_t k) {
        return FCell { c, h, d, d + h * std::sqrt(2), k };
    };
    auto evalOne = [&](const Point& c, double h) {
        std::uint32_t k;
        const double d = blocked ? distBlockedAvx2(c.get<0>(), c.get<1>(), e, 0, k)
            : useAvx2() ? distOneAvx2(c.get<0>(), c.get<1>(), e, k) : distOneSse2(c.get<0>(), c.get<1>(), e, k);
        return makeCell(c, h, d, k);
    };

    // centroid (as getCentroidCell)
    Point centroid;
    {
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
        centroid = area == 0 ? ring.at(0) : Point { ox + cx / area, oy + cy / area };
    }
    const Point bboxCenter {
        envelope.min_corner().get<0>() + size.get<0>() / 2.0,
        envelope.min_corner().get<1>() + size.get<1>() / 2.0
    };
    double h = cellSize / 2;

    // Small polygons: the two first guesses and the initial grid cells are evaluated together, four
    // per kernel call (evaluation is pure; the decisions below follow in the original order).
    const bool combined = !bound && useAvx2();
    thread_local std::vector<Point> gridStore;
    thread_local std::vector<double> gridD;
    thread_local std::vector<std::uint32_t> gridK;
    std::vector<Point>& grid = gridStore;
    FCell centroidCell, bboxCell;
    if (combined) {
        grid.clear();
        grid.push_back(centroid);
        grid.push_back(bboxCenter);
        for (double x = envelope.min_corner().get<0>(); x < envelope.max_corner().get<0>(); x += cellSize) {
            for (double y = envelope.min_corner().get<1>(); y < envelope.max_corner().get<1>(); y += cellSize) {
                grid.push_back(Point {x + h, y + h});
            }
        }
        const std::size_t ng = grid.size();
        gridD.resize(ng + 4);
        gridK.resize(ng + 4);
        for (std::size_t i = 0; i < ng; i += 4) {
            alignas(32) double gx[4], gy[4], mx[4];
            for (std::size_t l = 0; l < 4; l++) {
                const Point& q = grid[i + l < ng ? i + l : i];
                gx[l] = q.get<0>();
                gy[l] = q.get<1>();
            }
            distFourAvx2(gx, gy, e, &gridD[i], &gridK[i], 0, 0, 0, mx);
        }
        centroidCell = makeCell(grid[0], 0, gridD[0], gridK[0]);
        bboxCell = makeCell(grid[1], 0, gridD[1], gridK[1]);
    } else {
        centroidCell = evalOne(centroid, 0);
        bboxCell = evalOne(bboxCenter, 0);
    }
    FCell bestCell = centroidCell;
    if (bboxCell.d > bestCell.d) bestCell = bboxCell;

    thread_local Heap heapStore;
    Heap& queue = heapStore;
    queue.clear();

    // no-op when popped: bestCell.d only grows (same test as provablyNoOp, on exact or bounded values)
    auto noOp = [&](double d, double max) {
        return d <= bestCell.d && max - bestCell.d <= precision;
    };
    auto boundNoOp = [&](double ub, double hc) {
        return ub <= bestCell.d && (ub + hc * sqrt2) - bestCell.d <= precision;
    };

    alignas(32) double bxs[4], bys[4], bd[4];
    std::uint32_t bk[4] = {}; // the small path does not track nearest edges (unused there)
    int bm = 0;
    double bh = 0;
    std::uint32_t bhint = 0;
    auto flush = [&]() {
        if (!bm) return;
        if (!blocked && bm > 1 && useAvx2()) {
            for (int l = bm; l < 4; l++) { bxs[l] = bxs[0]; bys[l] = bys[0]; }
            alignas(32) double bmx[4];
            // k is only needed by the edge bound, which this path does not use
            int keep = distFourAvx2T<false>(bxs, bys, e, bd, bk, bh * std::sqrt(2), bestCell.d, precision, bmx) & ((1 << bm) - 1);
            while (keep) {
                const int l = __builtin_ctz(keep);
                keep &= keep - 1;
                queue.push(FCell { Point { bxs[l], bys[l] }, bh, bd[l], bmx[l], bk[l] });
            }
            bm = 0;
            return;
        }
        if (blocked) {
            for (int l = 0; l < bm; l++) bd[l] = distBlockedAvx2(bxs[l], bys[l], e, bhint, bk[l]);
        } else {
            distMany(bxs, bys, bm, e, bd, bk);
        }
        for (int l = 0; l < bm; l++) {
            const FCell c = makeCell(Point { bxs[l], bys[l] }, bh, bd[l], bk[l]);
            if (noOp(c.d, c.max)) continue;
            queue.push(c);
        }
        bm = 0;
    };
    auto add = [&](double x, double y, double hc) {
        bxs[bm] = x; bys[bm] = y; bh = hc; bm++;
        if (bm == 4) flush();
    };

    // cover polygon with initial cells
    bhint = bboxCell.k;
    if (combined) {
        for (std::size_t i = 2, ng = grid.size(); i < ng; i++) {
            const FCell c = makeCell(grid[i], h, gridD[i], gridK[i]);
            if (!noOp(c.d, c.max)) queue.push(c);
        }
        if (grid.capacity() > (1u << 12)) {
            std::vector<Point>().swap(grid);
            std::vector<double>().swap(gridD);
            std::vector<std::uint32_t>().swap(gridK);
        }
    } else for (double x = envelope.min_corner().get<0>(); x < envelope.max_corner().get<0>(); x += cellSize) {
        for (double y = envelope.min_corner().get<1>(); y < envelope.max_corner().get<1>(); y += cellSize) {
            const Point p {x + h, y + h};
            if (bound) {
                const double ub = std::min(edgeBound(p, e, bboxCell.k), edgeBound(p, e, centroidCell.k));
                if (boundNoOp(ub, h)) continue;
            }
            add(p.get<0>(), p.get<1>(), h);
        }
    }
    flush();

    while (!queue.empty()) {
        const FCell cell = queue.pop();

        if (cell.d > bestCell.d) bestCell = cell;

        if (cell.max - bestCell.d <= precision) {
            // no remaining cell is split: the result is the first cell in pop order with the
            // largest d > bestCell.d
            const FCell* cand = nullptr;
            queue.forEach([&](const FCell& c) {
                if (c.d > bestCell.d && (!cand || c.d > cand->d || (c.d == cand->d && lessCell(*cand, c)))) cand = &c;
            });
            if (cand) bestCell = *cand;
            break;
        }

        h = cell.h / 2;
        bhint = cell.k;
        const Point children[4] = {
            {cell.c.get<0>() - h, cell.c.get<1>() - h},
            {cell.c.get<0>() + h, cell.c.get<1>() - h},
            {cell.c.get<0>() - h, cell.c.get<1>() + h},
            {cell.c.get<0>() + h, cell.c.get<1>() + h}
        };
        for (const Point& p : children) {
            if (bound && boundNoOp(edgeBound(p, e, cell.k), h)) continue;
            add(p.get<0>(), p.get<1>(), h);
        }
        flush();
    }

    queue.release();
    return bestCell.c;
}

// polylabel() for polygons with fewer than kBoundMinEdges edges on AVX2 CPUs, in one pass over
// the rings: the envelope of the outer ring (as geom::envelope: the first point, then strict < and
// > per coordinate), the early return for cellSize == 0, the edge arrays of Edges::build (same
// entries; the order of the edges changes neither the minimum nor the parity) on the stack, the
// centroid (as getCentroidCell, same operations in the same order), then polylabelSmallAvx2.
// Returns false, having decided nothing, if the polygon is not small, the cell size is not finite,
// a coordinate is not finite or of magnitude >= 1e100, or the small path declines.
__attribute__((target("avx2")))
inline bool polylabelSmall(const Polygon& polygon, double precision, Point& out) {
    const auto& outer = polygon.outer();
    std::size_t n = outer.size();
    for (const auto& inner : polygon.inners()) n += inner.size();
    if (n >= kBoundMinEdges || outer.empty()) return false;
    alignas(32) double buf[7][kBoundMinEdges];
    Edges e;
    e.ax = buf[0]; e.ay = buf[1]; e.bx = buf[2]; e.by = buf[3]; e.dx = buf[4]; e.dy = buf[5]; e.den = buf[6];
    e.n = n;
    // non-degenerate edges from the front, zero-length ones from the back
    std::size_t w = 0, z = n;
    bool ok = true;
    auto edge = [&](double x, double y, double bx, double by) {
        ok &= std::fabs(x) < 1e100 && std::fabs(y) < 1e100;
        const double dx = bx - x, dy = by - y;
        const std::size_t k = dx == 0 && dy == 0 ? --z : w++;
        e.ax[k] = x; e.ay[k] = y; e.bx[k] = bx; e.by[k] = by; e.dx[k] = dx; e.dy[k] = dy;
        e.den[k] = dx * dx + dy * dy;
    };
    const double ox = outer[0].get<0>();
    const double oy = outer[0].get<1>();
    double minX = ox, minY = oy, maxX = ox, maxY = oy;
    double area = 0, cx = 0, cy = 0;
    for (std::size_t i = 0, len = outer.size(), j = len - 1; i < len; j = i++) {
        const double x = outer[i].get<0>(), y = outer[i].get<1>();
        const double bx = outer[j].get<0>(), by = outer[j].get<1>();
        if (x < minX) minX = x;
        if (x > maxX) maxX = x;
        if (y < minY) minY = y;
        if (y > maxY) maxY = y;
        edge(x, y, bx, by);
        const double ax = x - ox;
        const double ay = y - oy;
        const double bxo = bx - ox;
        const double byo = by - oy;
        auto f = ax * byo - bxo * ay;
        cx += (ax + bxo) * f;
        cy += (ay + byo) * f;
        area += f * 3;
    }
    const Box envelope { Point { minX, minY }, Point { maxX, maxY } };
    const Point size { maxX - minX, maxY - minY };
    const double cellSize = std::min(size.get<0>(), size.get<1>());
    if (cellSize == 0) {
        out = envelope.min_corner();
        return true;
    }
    if (!std::isfinite(cellSize)) return false;
    for (const auto& inner : polygon.inners()) {
        for (std::size_t i = 0, len = inner.size(), j = len - 1; i < len; j = i++) {
            edge(inner[i].get<0>(), inner[i].get<1>(), inner[j].get<0>(), inner[j].get<1>());
        }
    }
    if (!ok) return false;
    e.nn = w;
    const Point centroid = area == 0 ? outer[0] : Point { ox + cx / area, oy + cy / area };
    const Point bboxCenter {
        envelope.min_corner().get<0>() + size.get<0>() / 2.0,
        envelope.min_corner().get<1>() + size.get<1>() / 2.0
    };
    return polylabelSmallAvx2(precision, envelope, cellSize, e, centroid, bboxCenter, out);
}

} // namespace fast
#endif

} // namespace detail

Point polylabel(const Polygon& polygon, double precision = 0.00001, bool debug = false) {
    using namespace detail;

#ifdef POLYLABEL_SSE2
    // small polygons on AVX2 CPUs: everything below in one pass, same result
    if (!debug && fast::useAvx2()) {
        Point r;
        if (fast::polylabelSmall(polygon, precision, r)) return r;
    }
#endif

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
    // all real input (finite coordinates of magnitude < 1e100): the fast path, same result
    if (!debug && std::isfinite(cellSize)) {
        thread_local fast::Edges fastEdges;
        if (fastEdges.build(polygon)) {
            struct Release {
                ~Release() { fastEdges.release(); }
            } release;
            return fast::polylabelFast(polygon, precision, envelope, size, cellSize, fastEdges);
        }
    }
#endif

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
