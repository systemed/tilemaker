/*! \file */
#ifndef _CLIP_LINE_H
#define _CLIP_LINE_H

#include <cmath>
#include <cstdint>
#include "geom.h"
#ifdef __SSE2__
#include <emmintrin.h>
#endif

// buildWayGeometry for a linestring: splits `ls` into the runs of segments that intersect
// clippingBox (appended to `out`), and returns whether any point lies outside extBox (then the
// caller intersects the result with extBox).
//
// Reference version: a segment with both ends within the closed clippingBox always intersects
// it (Boost's cartesian segment/box disjoint test then sees t ranges that contain the whole
// segment in both dimensions: every ti_min <= 0 <= ti_max, diff >= 0), so Boost isn't called for
// those. Every other segment is tested with boost::geometry::intersects.
template<class LS>
bool split_linestring_for_tile_reference(const LS &ls, Box const &clippingBox, Box const &extBox, MultiLinestring &out) {
	const double minX = extBox.min_corner().x(), maxX = extBox.max_corner().x();
	const double minY = extBox.min_corner().y(), maxY = extBox.max_corner().y();
	auto pointInsideExtBox = [minX, maxX, minY, maxY](const Point& p) {
		return p.x() >= minX && p.x() <= maxX && p.y() >= minY && p.y() <= maxY;
	};
	const double cx0 = clippingBox.min_corner().x(), cx1 = clippingBox.max_corner().x();
	const double cy0 = clippingBox.min_corner().y(), cy1 = clippingBox.max_corner().y();
	auto coveredByClip = [cx0, cx1, cy0, cy1](const Point& p) {
		return p.x() >= cx0 && p.x() <= cx1 && p.y() >= cy0 && p.y() <= cy1;
	};
	bool needsIntersection = !pointInsideExtBox(ls[0]);

	Linestring current_ls;
	current_ls.push_back(ls[0]);

	bool prevCovered = coveredByClip(ls[0]);
	for (size_t i = 1; i < ls.size(); ++i) {
		const bool covered = coveredByClip(ls[i]);
		if (!(prevCovered && covered)) {
			boost::geometry::model::segment<Point> segment(ls[i-1], ls[i]);
			if (!boost::geometry::intersects(segment, clippingBox)) {
				if (current_ls.size() > 1)
					out.push_back(std::move(current_ls));
				current_ls.clear();
			}
		}
		prevCovered = covered;
		current_ls.push_back(ls[i]);
		if (!needsIntersection)
			needsIntersection = !pointInsideExtBox(ls[i]);
	}

	if (current_ls.size() > 1)
		out.push_back(std::move(current_ls));
	return needsIntersection;
}

// Fast version, same result. Per point, one SSE2 pass computes whether it is within the closed
// clippingBox / extBox and on which sides it lies more than M outside clippingBox. A segment
// then
//  - continues the run if both ends are within clippingBox (as in the reference);
//  - ends it if both ends are more than M left (right) of the box: Boost's x step returns
//    disjoint. With the ends left, either p1 >= p0 and fl(x0 - p0) > fl(p1 - p0) (the exact
//    values differ by x0 - p1 > M, far more than their rounding errors), or p1 < p0 and
//    fl(p0 - x0) < 0. Neither value is near enough to 0 for the diff ~ 0 branch to replace it.
//    Right: symmetric;
//  - ends it if both ends are more than M below (above) the box, and the x extent s = |p1x - p0x|
//    (as Boost computes it) is either exactly 0 or at least D: if the x step passes
//    (t_min.first <= s, t_max.first >= 0), the y step either returns disjoint at once or updates
//    t_min to (ti_min, dy) with ti_min > dy (below, p1y >= p0y: the products ti_min*s and
//    t_min.first*dy differ by at least M*s, more than their relative rounding errors), resp.
//    t_max to (ti_max, dy) with ti_max < 0 (below, p1y < p0y: ti_max*s < 0 <= t_max.first*dy,
//    no underflow as s >= D), and then returns disjoint. With s exactly 0, the x step leaves
//    t_min.first < 0 < t_max.first (it replaces values ~0 by -1/1), and the same updates
//    happen. Above: symmetric;
//  - is tested with boost::geometry::intersects otherwise, as in the reference.
// The margins need bounded coordinates (|c| <= BOUND); if the box or any point (NaN included)
// isn't, the reference version runs instead. Each run is copied into its part in one go (only
// the capacity of the parts differs from appending point by point).
namespace clip_line_detail {
	constexpr double M = 1e-9, D = 1e-12, BOUND = 1000.0;
}

template<class LS>
bool split_linestring_for_tile(const LS &ls, Box const &clippingBox, Box const &extBox, MultiLinestring &out) {
#ifdef __SSE2__
	using namespace clip_line_detail;
	const size_t n = ls.size();
	const double cx0 = clippingBox.min_corner().x(), cx1 = clippingBox.max_corner().x();
	const double cy0 = clippingBox.min_corner().y(), cy1 = clippingBox.max_corner().y();
	if (!(std::fabs(cx0) <= BOUND && std::fabs(cx1) <= BOUND && std::fabs(cy0) <= BOUND && std::fabs(cy1) <= BOUND))
		return split_linestring_for_tile_reference(ls, clippingBox, extBox, out);
	const size_t outSize = out.size();

	const __m128d cmin = _mm_set_pd(cy0, cx0), cmax = _mm_set_pd(cy1, cx1);
	const __m128d emin = _mm_set_pd(extBox.min_corner().y(), extBox.min_corner().x());
	const __m128d emax = _mm_set_pd(extBox.max_corner().y(), extBox.max_corner().x());
	const __m128d smin = _mm_set_pd(cy0 - M, cx0 - M), smax = _mm_set_pd(cy1 + M, cx1 + M);
	const __m128d absMask = _mm_castsi128_pd(_mm_set1_epi64x(0x7fffffffffffffffLL)), bound = _mm_set1_pd(BOUND);
	__m128d bounded = _mm_castsi128_pd(_mm_set1_epi32(-1));
	// code bits (two per compare): 0-3 within clippingBox (x, y), 4-7 within extBox, 8-9 left,
	// 10-11 below, 12-13 right, 14-15 above
	auto code = [&](const Point &p) -> unsigned {
		const __m128d v = _mm_loadu_pd(reinterpret_cast<const double*>(&p));
		bounded = _mm_and_pd(bounded, _mm_cmple_pd(_mm_and_pd(v, absMask), bound));
		const __m128i in = _mm_castpd_si128(_mm_and_pd(_mm_cmpge_pd(v, cmin), _mm_cmple_pd(v, cmax)));
		const __m128i inExt = _mm_castpd_si128(_mm_and_pd(_mm_cmpge_pd(v, emin), _mm_cmple_pd(v, emax)));
		const __m128i lo = _mm_castpd_si128(_mm_cmplt_pd(v, smin)), hi = _mm_castpd_si128(_mm_cmpgt_pd(v, smax));
		return unsigned(_mm_movemask_epi8(_mm_packs_epi16(_mm_packs_epi32(in, inExt), _mm_packs_epi32(lo, hi))));
	};
	constexpr unsigned C_COV = 0xf, C_EXT = 0xf0, C_LR = 0x100 | 0x1000, C_BT = 0x400 | 0x4000;

	unsigned prev = code(ls[0]);
	unsigned all = prev;
	size_t start = 0;
	for (size_t i = 1; i < n; ++i) {
		const unsigned cur = code(ls[i]);
		all &= cur;
		const unsigned both = prev & cur;
		prev = cur;
		if ((both & C_COV) == C_COV) continue;
		if (!(both & C_LR)) {
			bool sure = false;
			if (both & C_BT) {
				const double x0 = ls[i-1].x(), x1 = ls[i].x();
				const double s = x1 >= x0 ? x1 - x0 : x0 - x1;
				sure = s == 0 || s >= D;
			}
			if (!sure && boost::geometry::intersects(boost::geometry::model::segment<Point>(ls[i-1], ls[i]), clippingBox))
				continue;
		}
		// segment (i-1, i) doesn't intersect the box: the run so far ends at i-1
		if (i - 1 > start)
			out.emplace_back(ls.begin() + start, ls.begin() + i);
		start = i;
	}
	if (_mm_movemask_pd(bounded) != 3) {
		// a point out of range (or NaN): the shortcuts may not match Boost
		out.resize(outSize);
		return split_linestring_for_tile_reference(ls, clippingBox, extBox, out);
	}
	if (n - 1 > start)
		out.emplace_back(ls.begin() + start, ls.end());
	return (all & C_EXT) != C_EXT;
#else
	return split_linestring_for_tile_reference(ls, clippingBox, extBox, out);
#endif
}

// buildWayGeometry LINESTRING_: the parts of `ls` for the tile
template<class LS>
MultiLinestring clip_linestring_for_tile(const LS &ls, Box const &clippingBox, Box const &extBox) {
	MultiLinestring out;
	if (ls.empty())
		return out;
	if (!split_linestring_for_tile(ls, clippingBox, extBox, out))
		return out;
	MultiLinestring result;
	boost::geometry::intersection(out, extBox, result);
	return result;
}

#endif //_CLIP_LINE_H
