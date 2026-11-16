#ifndef __BOOST_GEOMETRY_DISSOLVE_H__
#define __BOOST_GEOMETRY_DISSOLVE_H__

/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Wouter van Kleunen wrote this file.  As long as you retain this notice you
 * can do whatever you want with this stuff. If we meet some day, and you think
 * this stuff is worth it, you can buy me a beer in return. 
 * ----------------------------------------------------------------------------
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#include <boost/geometry.hpp>
#include <boost/version.hpp>
#include "geom.h"
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/geometries/polygon.hpp>
#include <boost/geometry/geometries/multi_polygon.hpp>
#include <boost/geometry/index/rtree.hpp>
#include <boost/function_output_iterator.hpp>

namespace geometry {

namespace impl {

// Conservative tests (geom.h specializes this for MultiPolygon with the precheck of geom.cpp); false
// means unsure: valid(mp), mp is certainly valid; apart(a, b), the polygons certainly don't intersect
template<typename multi_polygon_t>
struct precheck
{
	using polygon_t = typename boost::range_value<multi_polygon_t>::type;
	static bool valid(multi_polygon_t const &) { return false; }
	static bool apart(polygon_t const &, polygon_t const &) { return false; }
};

// boxes[i] is the envelope of result[i]. Polygons whose envelopes are disjoint
// cannot intersect, so the box test skips most of the costly intersects() calls
// (otherwise quadratic in the number of rings, e.g. a lake with 14000 islands).
template<typename C, typename B, typename T>
static inline void result_combine(C &result, std::vector<B> &boxes, T &&new_element)
{
    result.push_back(std::forward<T>(new_element));
    boxes.push_back(boost::geometry::return_envelope<B>(result.back()));

   	for(std::size_t i = 0; i < result.size() - 1; ) {
        if(boost::geometry::disjoint(boxes[i], boxes.back()) ||
           precheck<C>::apart(result[i], result.back()) ||
           !boost::geometry::intersects(result[i], result.back())) {
            ++i;
            continue;
        }

        std::vector<T> union_result;
        boost::geometry::union_(result[i], result.back(), union_result);

        if(union_result.size() != 1) {
			++i;
			continue;
		}

       	result.back() = std::move(union_result[0]);
       	boxes.back() = boost::geometry::return_envelope<B>(result.back());
       	result.erase(result.begin() + i);
       	boxes.erase(boxes.begin() + i);
    }
}

// boost::geometry::difference(outers, holes, output) for one outer without holes and holes without
// holes, when the polygon of the outer and the holes reversed (in order) is certainly valid by
// precheck::valid (clear of each other: Boost finds no turns between the outer and the holes;
// each hole clearly inside the outer). false, nothing done: not that case. Boost's overlay then
// keeps the outer (its point is in no hole) and every hole (its point is in the outer), reversed;
// assign_parents makes the outer the parent of each hole (positive outer, negative holes, the
// outer's area the larger; the hole's point in the outer's box and ring); add_rings adds the outer
// with the holes as children in ring id order, i.e. the holes' order, if the polygon has 4 points
// or more and an area positive and not 0 by math::equals.
template<typename multi_polygon_t>
static inline bool difference_inside(multi_polygon_t const &outers, multi_polygon_t const &holes, multi_polygon_t &output)
{
	using polygon_t = typename boost::range_value<multi_polygon_t>::type;
	if(outers.size() != 1 || holes.empty() || !outers[0].inners().empty()) return false;
	auto const outer_area = boost::geometry::area(outers[0].outer());
	using area_t = decltype(outer_area);
	if(!boost::geometry::math::larger(outer_area, area_t(0))) return false;
	for(auto const &hole: holes) {
		if(!hole.inners().empty()) return false;
		auto const hole_area = boost::geometry::area(hole.outer());
		if(!boost::geometry::math::larger(hole_area, area_t(0)) || outer_area < hole_area) return false;
	}
	multi_polygon_t candidate;
	candidate.resize(1);
	polygon_t &result = candidate[0];
	result.outer() = outers[0].outer();
	result.inners().resize(holes.size());
	for(std::size_t i = 0; i < holes.size(); i++)
		result.inners()[i].assign(holes[i].outer().rbegin(), holes[i].outer().rend());
	if(!precheck<multi_polygon_t>::valid(candidate)) return false;
	if(boost::geometry::num_points(result) >= 4) {
		auto const area = boost::geometry::area(result);
		if(!boost::geometry::math::equals(area, decltype(area)(0)) && area > 0)
			output.push_back(std::move(result));
	}
	return true;
}

// boost::geometry::difference(outers, empty, output) for outers whose polygons have no holes and
// rings of positive area (false, nothing done: not that case); outers is moved from
template<typename multi_polygon_t>
static inline bool difference_without_holes(multi_polygon_t &outers, multi_polygon_t &output)
{
	for(auto const &poly: outers) {
		if(!poly.inners().empty() || boost::size(poly.outer()) == 0) return false;
		if(!(boost::geometry::area(poly.outer()) > 0)) return false;
	}
	for(auto &poly: outers) {
		if(boost::size(poly.outer()) < 4) continue;
		auto const area = boost::geometry::area(poly);
		if(boost::geometry::math::equals(area, decltype(area)(0)) || !(area > 0)) continue;
		output.push_back(std::move(poly));
	}
	return true;
}

struct pseudo_vertice_key
{
    std::size_t index_1;
    double scale;
    std::size_t index_2;
    bool reroute;
    
    pseudo_vertice_key(std::size_t index_1 = 0, std::size_t index_2 = 0, double scale = 0.0, bool reroute = false)
        : index_1(index_1), scale(scale), index_2(index_2), reroute(reroute)
    { } 
};

struct compare_pseudo_vertice_key
{
    bool operator()(pseudo_vertice_key const &a, pseudo_vertice_key const &b) const {
        if(a.index_1 < b.index_1) return true;
        if(a.index_1 > b.index_1) return false;
        if(a.scale < b.scale) return true;
        if(a.scale > b.scale) return false;
        if(a.index_2 > b.index_2) return true;
        if(a.index_2 < b.index_2) return false;
        if(a.reroute && !b.reroute) return true;
        if(!a.reroute && b.reroute) return false;
		return false;
    }
};

template<typename point_t = boost::geometry::model::d2::point_xy<double>>
struct pseudo_vertice
{
    point_t p;
    pseudo_vertice_key link;
    
    pseudo_vertice(point_t p, pseudo_vertice_key link = pseudo_vertice_key())   
        : p(p), link(link)
    { }        
};

template<
	typename point_t = boost::geometry::model::d2::point_xy<double>, 
	typename ring_t = boost::geometry::model::ring<point_t>
	>
static inline void dissolve_find_intersections(
			ring_t const &ring,
			std::map<pseudo_vertice_key, pseudo_vertice<point_t>, compare_pseudo_vertice_key> &pseudo_vertices,
    		std::set<pseudo_vertice_key, compare_pseudo_vertice_key> &start_keys)
{
	if(ring.empty()) return;

	boost::geometry::index::rtree<std::pair< boost::geometry::model::segment<point_t>, std::size_t >, boost::geometry::index::quadratic<16>> index;
	std::vector<point_t> output;
	output.reserve(2);

	// Generate all by-pass intersections in the graph
	// Generate a list of all by-pass intersections
    pseudo_vertices.emplace(pseudo_vertice_key(ring.size() - 1, ring.size() - 1, 0.0), ring.back());       
    for(std::size_t i = ring.size() - 1; i--; )
    {
        pseudo_vertices.emplace(pseudo_vertice_key(i, i, 0.0), ring[i]);       
		boost::geometry::model::segment<point_t> line_1(ring[i], ring[i + 1]);

		boost::geometry::index::query(
				index, boost::geometry::index::intersects(line_1), 
				boost::make_function_output_iterator([&](std::pair< boost::geometry::model::segment<point_t>, std::size_t > const &iter) {

			auto const &line_2 = iter.first;
			auto j = iter.second;
			
			output.clear();
			boost::geometry::intersection(line_1, line_2, output);

			for(auto const &p: output) {
				double scale_1 = boost::geometry::comparable_distance(p, ring[i]) / boost::geometry::comparable_distance(ring[i + 1], ring[i]);
				double scale_2 = boost::geometry::comparable_distance(p, ring[j]) / boost::geometry::comparable_distance(ring[j + 1], ring[j]);
				if(scale_1 < 1.0 && scale_2 < 1.0) {
					pseudo_vertice_key key_j(j, i, scale_2);
					pseudo_vertices.emplace(pseudo_vertice_key(i, j, scale_1, true), pseudo_vertice<point_t>(p, key_j));
					pseudo_vertices.emplace(key_j, p);
					start_keys.insert(key_j);

					pseudo_vertice_key key_i(i, j, scale_1);
					pseudo_vertices.emplace(pseudo_vertice_key(j, i, scale_2, true), pseudo_vertice<point_t>(p, key_i));
					pseudo_vertices.emplace(key_i, p);
					start_keys.insert(key_i);
				}
			}          
		}));

		index.insert(std::make_pair(boost::geometry::model::segment<point_t>(ring[i], ring[i+1]), i));
    }
}

// A self-intersection of a ring: segments i < j (segment k runs ring[k] -> ring[k + 1]) meet in p,
// at scale_1 along segment i and scale_2 along segment j (comparable distances, both below 1)
template<typename point_t>
struct dissolve_crossing
{
	std::size_t i, j;
	double scale_1, scale_2;
	point_t p;
};

// Boost's intersection of the segments (a, s) and (s, c), which meet in s, when they are no points
// and not collinear: then Boost computes the point from the ratios along the segments (cramers_rule,
// segment_intersection_info::calculate). That is done here with the same Boost functions and the
// same values, without the side tests Boost runs before (two of the four sides involve s and are 0,
// so the segments are never disjoint; collinear needs all four 0, so side(s, c, a) != 0 rules it
// out). -1: not that case (or not sure), use boost::geometry::intersection; 0: the point is s; 1:
// the point is ip.
// (Not with FMA: the products could be contracted differently from Boost's.)
#if !defined(__FMA__) && !defined(__FMA4__) && (!defined(__FLT_EVAL_METHOD__) || __FLT_EVAL_METHOD__ == 0)
#define DISSOLVE_ADJACENT_FAST 1
#endif
template<typename point_t>
static inline int dissolve_adjacent_intersection(point_t const &a, point_t const &s, point_t const &c, point_t &ip)
{
	namespace bg = boost::geometry;
	using coordinate_t = typename bg::coordinate_type<point_t>::type;
	using strategy_t = bg::strategy::intersection::cartesian_segments<>;
	using ratio_t = bg::segment_ratio<coordinate_t>;
	if(!std::is_same<coordinate_t, double>::value) return -1;
	const double eps = std::numeric_limits<double>::epsilon();
	// math::equals (finite values), point by point as cartesian_point_point
	auto equal = [eps](double x, double y) { return x == y || std::abs(x - y) <= eps * std::max({ std::abs(x), std::abs(y), 1.0 }); };
	auto same_point = [&](point_t const &u, point_t const &v) { return equal(bg::get<0>(u), bg::get<0>(v)) && equal(bg::get<1>(u), bg::get<1>(v)); };
	// unified(): p = (a, s), q = (s, c); neither is a point
	if(same_point(a, s) || same_point(s, c))
		return -1;
	coordinate_t const dx_p = bg::get<0>(s) - bg::get<0>(a);
	coordinate_t const dx_q = bg::get<0>(c) - bg::get<0>(s);
	coordinate_t const dy_p = bg::get<1>(s) - bg::get<1>(a);
	coordinate_t const dy_q = bg::get<1>(c) - bg::get<1>(s);
	// sides: side(s, c, a), side(s, c, s) = 0, side(a, s, s) = 0, side(a, s, c): not all 0.
	// side_by_triangle(s, c, a) is 0 when two of the points are equal (math::equals), else it is
	// a determinant of two of the differences from one of the points (picked by their order),
	// 0 when within epsilon * max(1, its differences). Each such determinant is within
	// 6.02 u m^2 (u = eps / 2, m the largest coordinate difference) of the exact one, as is t
	// below: |t| > 16 eps (m^2 + max(1, m)) leaves every one of them far from 0.
	{
		coordinate_t const dx_ac = bg::get<0>(c) - bg::get<0>(a), dy_ac = bg::get<1>(c) - bg::get<1>(a);
		coordinate_t const t = dx_q * (bg::get<1>(a) - bg::get<1>(s)) - dy_q * (bg::get<0>(a) - bg::get<0>(s));
		coordinate_t const m = std::max({ std::abs(dx_p), std::abs(dy_p), std::abs(dx_q), std::abs(dy_q), std::abs(dx_ac), std::abs(dy_ac) });
		const bool side_clear = !same_point(a, c) && std::abs(t) > 16 * eps * (m * m + std::max(1.0, m));
		if(!side_clear && bg::strategy::side::side_by_triangle<>::apply(s, c, a) == 0)
			return -1;
	}
	coordinate_t denominator_a, nominator_a;
	coordinate_t denominator_b, nominator_b;
	strategy_t::cramers_rule(dx_p, dy_p, dx_q, dy_q,
		bg::get<0>(a) - bg::get<0>(s), bg::get<1>(a) - bg::get<1>(s),
		nominator_a, denominator_a);
	strategy_t::cramers_rule(dx_q, dy_q, dx_p, dy_p,
		bg::get<0>(s) - bg::get<0>(a), bg::get<1>(s) - bg::get<1>(a),
		nominator_b, denominator_b);
	bg::math::detail::equals_factor_policy<coordinate_t> policy(dx_p, dy_p, dx_q, dy_q);
	coordinate_t const zero = 0;
	// (Boost takes these as collinear)
	if(bg::math::detail::equals_by_policy(denominator_a, zero, policy)
	 || bg::math::detail::equals_by_policy(denominator_b, zero, policy))
		return -1;
	// calculate() takes the point from segment a or b. From b it is s: b's ratio is 0 / d
	// (nominator_b is dx_p * dy_p - dy_p * dx_p), s + 0 * dx / d. When the one from a (as
	// assign(): a + numerator * dx / denominator, the ratio's sign normalized) is s as well, the
	// point is s either way (and the nearly collinear correction leaves s, the end of both).
	{
		coordinate_t numerator = nominator_a, denominator = denominator_a;
		if(denominator < zero) { numerator = -numerator; denominator = -denominator; }
		coordinate_t const x = bg::get<0>(a) + numerator * dx_p / denominator;
		coordinate_t const y = bg::get<1>(a) + numerator * dy_p / denominator;
		if(x == bg::get<0>(s) && y == bg::get<1>(s))
			return 0;
	}
	typename strategy_t::template segment_intersection_info<coordinate_t, ratio_t> sinfo;
	sinfo.dx_a = dx_p;
	sinfo.dx_b = dx_q;
	sinfo.dy_a = dy_p;
	sinfo.dy_b = dy_q;
#if BOOST_VERSION >= 108700
	sinfo.ra.assign(nominator_a, denominator_a);
	sinfo.rb.assign(nominator_b, denominator_b);
#else
	sinfo.robust_ra.assign(nominator_a, denominator_a);
	sinfo.robust_rb.assign(nominator_b, denominator_b);
#endif
	bg::model::referring_segment<point_t const> const p(a, s);
	bg::model::referring_segment<point_t const> const q(s, c);
	sinfo.calculate(ip, p, q);
	return 1;
}

// Whether Boost's intersects(box of (s, c), segment (a, s)) certainly holds, given s - a: when each
// coordinate difference is 0 or not 0 by math::equals (see dissolve_find_crossings)
static inline bool dissolve_envelope_holds(double dx, double dy)
{
	const double eps = std::numeric_limits<double>::epsilon();
	return (dx == 0 || std::abs(dx) > eps) && (dy == 0 || std::abs(dy) > eps);
}

// The crossings dissolve_find_intersections finds, without its rtree, which it fills segment by
// segment (that cost the most). It reports the pairs of segments i < j for which
// intersects(segment j, segment i) holds and, while it queries segment i, the boxes of the rtree
// nodes above segment j intersect segment i. Those boxes contain the envelope of segment j: when
// segment i intersects that envelope, it intersects them too, and the pair is reported. When it
// doesn't (segment j only just touches segment i, within Boost's tolerance), whether the pair is
// reported depends on the shape of the rtree: then false (nothing is known, use
// dissolve_find_intersections). The pairs are taken from boxes that overlap with a margin far
// above that tolerance, and the order of the pairs doesn't matter (each gives its own pseudo
// vertex keys).
template<
	typename point_t = boost::geometry::model::d2::point_xy<double>,
	typename ring_t = boost::geometry::model::ring<point_t>
	>
static inline bool dissolve_find_crossings(ring_t const &ring, std::vector<dissolve_crossing<point_t>> &crossings)
{
	if(ring.size() < 2) return true;
	using segment_t = boost::geometry::model::segment<point_t>;
	using box_t = boost::geometry::model::box<boost::geometry::model::point<double, 2, boost::geometry::cs::cartesian>>;
	const std::size_t segments = ring.size() - 1;

	struct seg_box { double min_x, max_x, min_y, max_y; };
	std::vector<seg_box> boxes(segments);
	for(std::size_t k = 0; k < segments; k++) {
		const double x0 = boost::geometry::get<0>(ring[k]), y0 = boost::geometry::get<1>(ring[k]);
		const double x1 = boost::geometry::get<0>(ring[k + 1]), y1 = boost::geometry::get<1>(ring[k + 1]);
		if(!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1)) return false;
		const double margin = 1e-9 * std::max({ 1.0, std::abs(x0), std::abs(y0), std::abs(x1), std::abs(y1) });
		boxes[k] = seg_box{ std::min(x0, x1) - margin, std::max(x0, x1) + margin, std::min(y0, y1) - margin, std::max(y0, y1) + margin };
	}

	std::vector<point_t> output;
	output.reserve(2);
	// segments i < j whose boxes overlap; false: not decided (see above)
	auto pair = [&](std::size_t i, std::size_t j) {
		segment_t line_1(ring[i], ring[i + 1]);
		segment_t line_2(ring[j], ring[j + 1]);
		output.clear();
#ifdef DISSOLVE_ADJACENT_FAST
		if(j == i + 1) {
			// Consecutive segments (a, s) and (s, c). Boost's intersection of them gives no crossing
			// (no point with both scales below 1) when one is exactly a point (a repeated point) or
			// when the first is at least as long as the second (cla >= clb, Boost's comparable
			// lengths, which are also the comparable distances below):
			//  - collinear (all sides 0, or a denominator 0 by Boost's tolerance): its points are
			//    end points, a, s or c; at a, scale 2 is cla / clb (>= 1), at s scale 1 is 1, at c
			//    scale 2 is 1 (0 / 0 for a point: NaN). With a point segment, the point is a (both
			//    points; a = s or scale 2 cla / clb or x / 0) or the point segment's first point.
			//  - else Boost computes it from segment b = (s, c) (use_a: edge value of b's ratio
			//    0 / d is 1, a's at most 1, a's relative length 0): s + 0 * dx / d = s, which the
			//    nearly collinear correction leaves; scale 1 is 1.
			// Then the pair adds nothing; it is undecided only when Boost's intersects(line_2,
			// line_1) holds and the envelope test fails.
			// The envelope test (disjoint_segment_box_impl, a slab test of line_1 = (a, s) against the
			// box of (s, c), which holds s) can only fail when a coordinate difference of line_1 is
			// 0 by math::equals without being 0: in each dimension the box's distances from a bracket
			// line_1's (ti_min <= diff <= ti_max, rounding is monotonic), so with diff 0 or clearly
			// not 0 every comparison it makes keeps t = 1 (s) inside.
			point_t const &a = ring[i], &s = ring[j], &c = ring[j + 1];
			const double ax = boost::geometry::get<0>(a), ay = boost::geometry::get<1>(a);
			const double sx = boost::geometry::get<0>(s), sy = boost::geometry::get<1>(s);
			const double cx = boost::geometry::get<0>(c), cy = boost::geometry::get<1>(c);
			const bool point_1 = ax == sx && ay == sy;
			const bool point_2 = sx == cx && sy == cy;
			const double dx_p = sx - ax, dy_p = sy - ay, dx_q = cx - sx, dy_q = cy - sy;
			const double cla = dx_p * dx_p + dy_p * dy_p, clb = dx_q * dx_q + dy_q * dy_q;
			const bool envelope_holds = dissolve_envelope_holds(dx_p, dy_p);
			if(point_1 || point_2 || cla >= clb)
				return envelope_holds
					|| boost::geometry::intersects(boost::geometry::return_envelope<box_t>(line_2), line_1)
					|| !boost::geometry::intersects(line_2, line_1);
			point_t ip;
			const int found = dissolve_adjacent_intersection(a, s, c, ip);
			if(found == 0)
				return envelope_holds
					|| boost::geometry::intersects(boost::geometry::return_envelope<box_t>(line_2), line_1);
			if(found > 0) {
				if(!envelope_holds && !boost::geometry::intersects(boost::geometry::return_envelope<box_t>(line_2), line_1)) return false;
				output.push_back(ip);
			}
		}
		if(output.empty()) {
#endif
		if(!boost::geometry::intersects(line_2, line_1)) return true;
		if(!boost::geometry::intersects(boost::geometry::return_envelope<box_t>(line_2), line_1)) return false;

		output.clear();
		boost::geometry::intersection(line_1, line_2, output);
#ifdef DISSOLVE_ADJACENT_FAST
		}
#endif

		for(auto const &p: output) {
			double scale_1 = boost::geometry::comparable_distance(p, ring[i]) / boost::geometry::comparable_distance(ring[i + 1], ring[i]);
			double scale_2 = boost::geometry::comparable_distance(p, ring[j]) / boost::geometry::comparable_distance(ring[j + 1], ring[j]);
			if(scale_1 < 1.0 && scale_2 < 1.0)
				crossings.push_back(dissolve_crossing<point_t>{ i, j, scale_1, scale_2, p });
		}
		return true;
	};
	auto overlap = [](seg_box const &a, seg_box const &b) {
		return a.min_x <= b.max_x && b.min_x <= a.max_x && a.min_y <= b.max_y && b.min_y <= a.max_y;
	};

	if(segments <= 32) {
		for(std::size_t i = 0; i < segments; i++)
			for(std::size_t j = i + 1; j < segments; j++)
				if(overlap(boxes[i], boxes[j]) && !pair(i, j)) return false;
		return true;
	}

	// Sweep over the boxes by their lower side along one axis: every pair of boxes that overlap
	// along it is looked at, and the overlapping ones are passed to pair (the order of the pairs
	// doesn't matter, see above). The axis is the one along which the boxes are narrower compared
	// to the ring's extent (fewer pairs to look at). A ring whose boxes overlap a lot along it (too
	// many pairs) is left to dissolve_find_intersections.
	double width = 0, height = 0, min_x = boxes[0].min_x, max_x = boxes[0].max_x, min_y = boxes[0].min_y, max_y = boxes[0].max_y;
	for(auto const &b: boxes) {
		width += b.max_x - b.min_x; height += b.max_y - b.min_y;
		min_x = std::min(min_x, b.min_x); max_x = std::max(max_x, b.max_x);
		min_y = std::min(min_y, b.min_y); max_y = std::max(max_y, b.max_y);
	}
	const bool along_y = height * (max_x - min_x) < width * (max_y - min_y);
	// sorted boxes, as (lo, hi) along the axis and (lo, hi) across, with their segment
	struct sweep_box { double lo, hi, across_lo, across_hi; uint32_t k; };
	std::vector<sweep_box> sorted(segments);
	for(std::size_t k = 0; k < segments; k++) {
		seg_box const &b = boxes[k];
		sorted[k] = along_y ? sweep_box{ b.min_y, b.max_y, b.min_x, b.max_x, uint32_t(k) }
		                    : sweep_box{ b.min_x, b.max_x, b.min_y, b.max_y, uint32_t(k) };
	}
	std::sort(sorted.begin(), sorted.end(), [](sweep_box const &a, sweep_box const &b) { return a.lo < b.lo; });
	std::size_t budget = 64 * segments + 4096;
	for(std::size_t a = 0; a < segments; a++) {
		const sweep_box ba = sorted[a];
		for(std::size_t b = a + 1; b < segments && sorted[b].lo <= ba.hi; b++) {
			if(!budget--) return false;
			const sweep_box &bb = sorted[b];
			if(bb.across_lo > ba.across_hi || ba.across_lo > bb.across_hi) continue;
			const std::size_t i = std::min(ba.k, bb.k), j = std::max(ba.k, bb.k);
			if(!pair(i, j)) return false;
		}
	}
	return true;
}

// The pseudo vertices of a ring with crossings, as dissolve_find_intersections adds them: its
// points, and two for each crossing (one on each segment) that link to each other
template<
	typename point_t = boost::geometry::model::d2::point_xy<double>,
	typename ring_t = boost::geometry::model::ring<point_t>
	>
static inline void dissolve_pseudo_vertices(
			ring_t const &ring,
			std::vector<dissolve_crossing<point_t>> const &crossings,
			std::map<pseudo_vertice_key, pseudo_vertice<point_t>, compare_pseudo_vertice_key> &pseudo_vertices,
    		std::set<pseudo_vertice_key, compare_pseudo_vertice_key> &start_keys)
{
	for(std::size_t i = ring.size(); i--; )
		pseudo_vertices.emplace(pseudo_vertice_key(i, i, 0.0), ring[i]);
	for(auto const &c: crossings) {
		const std::size_t i = c.i, j = c.j;
		pseudo_vertice_key key_j(j, i, c.scale_2);
		pseudo_vertices.emplace(pseudo_vertice_key(i, j, c.scale_1, true), pseudo_vertice<point_t>(c.p, key_j));
		pseudo_vertices.emplace(key_j, c.p);
		start_keys.insert(key_j);

		pseudo_vertice_key key_i(i, j, c.scale_1);
		pseudo_vertices.emplace(pseudo_vertice_key(j, i, c.scale_2, true), pseudo_vertice<point_t>(c.p, key_i));
		pseudo_vertices.emplace(key_i, c.p);
		start_keys.insert(key_i);
	}
}

// Remove invalid points (NaN) from ring
template<
	typename point_t = boost::geometry::model::d2::point_xy<double>, 
	typename ring_t = boost::geometry::model::ring<point_t>
	>
static inline void correct_invalid(ring_t &ring)
{
	for(auto i = ring.begin(); i != ring.end(); ) {
		if(!boost::geometry::is_valid(*i))
			i = ring.erase(i);
		else
			++i;
	}	
}

// Correct orientation of ring
template<
	typename point_t = boost::geometry::model::d2::point_xy<double>, 
	typename ring_t = boost::geometry::model::ring<point_t>
	>
static inline double correct_orientation(ring_t &ring, boost::geometry::order_selector order)
{
	auto area = boost::geometry::area(ring);
	bool should_reverse =
		(order == boost::geometry::clockwise && area < 0) ||
		(order == boost::geometry::counterclockwise && area > 0);

	if(should_reverse) {
		std::reverse(ring.begin(), ring.end());
	} 

	return area;
}

// Close ring if not closed
template<
	typename point_t = boost::geometry::model::d2::point_xy<double>, 
	typename ring_t = boost::geometry::model::ring<point_t>
	>
static inline void correct_close(ring_t &ring)
{
	// Close ring if not closed
	if(!ring.empty() && !boost::geometry::equals(ring.back(), ring.front()))
		ring.push_back(ring.front());

}

template<
	typename point_t = boost::geometry::model::d2::point_xy<double>, 
	typename ring_t = boost::geometry::model::ring<point_t>
	>
static inline std::vector<ring_t> dissolve_generate_rings(
			std::map<pseudo_vertice_key, pseudo_vertice<point_t>, compare_pseudo_vertice_key> &pseudo_vertices,
    		std::set<pseudo_vertice_key, compare_pseudo_vertice_key> &start_keys, 
			boost::geometry::order_selector order, double remove_spike_min_area = 0.0)
{
	std::vector<ring_t> result;

	// Generate all polygons by tracing all the intersections
	// Perform union to combine all polygons into single polygon again
	// Position of each point in the ring being traced. Its points are distinct until
	// it closes, so a point already seen closes it: looking that up here instead of
	// scanning the whole ring for every new point keeps tracing linear, not quadratic.
	struct point_hash {
		std::size_t operator()(std::pair<double, double> const &p) const {
			return std::hash<double>()(p.first) * 31 + std::hash<double>()(p.second);
		}
	};
	std::unordered_map<std::pair<double, double>, std::size_t, point_hash> seen;

    while(!start_keys.empty()) {
		ring_t new_ring;
		seen.clear();
		bool closed = false;

		// Store point in generated polygon; when it repeats an earlier point, the
		// ring is closed and the points before that one are dropped
		auto push_point = [&new_ring, &seen, &closed](auto const &p) {
            if(!new_ring.empty() && !(boost::geometry::comparable_distance(new_ring.back(), p) > 0))
                return;
			new_ring.push_back(p);
			// + 0.0 turns -0.0 into 0.0, which compares equal to it
			std::pair<double, double> key(boost::geometry::get<0>(p) + 0.0, boost::geometry::get<1>(p) + 0.0);
			auto found = seen.find(key);
			if(found == seen.end()) {
				seen.emplace(key, new_ring.size() - 1);
			} else {
				new_ring.erase(new_ring.begin(), new_ring.begin() + found->second);
				closed = true;
			}
		};

		auto is_closed = [&closed](ring_t &) { return closed; };

        auto start_iter = pseudo_vertices.find(*start_keys.begin());
        auto i = start_iter;
    
        do {
            auto const &key = i->first;
            auto const &value = i->second;
        
			// Store the point in output polygon
			push_point(value.p);
            
            start_keys.erase(key);
            if(key.reroute) {
				// Follow by-pass
                i = pseudo_vertices.find(value.link);
			} else {
				// Continu following original polygon
                ++i;
                if(i == pseudo_vertices.end())
                    i = pseudo_vertices.begin();
            }

			// Repeat until back at starting point
       	} while(!is_closed(new_ring));

		// Combine with already generated polygons
		auto area = boost::geometry::area(new_ring);
		if(std::abs(area) > remove_spike_min_area) {
	    	result.push_back(std::move(new_ring));
		}
   	}

    return result;
}

template<
	typename point_t = boost::geometry::model::d2::point_xy<double>, 
	typename polygon_t = boost::geometry::model::polygon<point_t>,
	typename ring_t = boost::geometry::model::ring<point_t>,
	typename multi_polygon_t = boost::geometry::model::multi_polygon<polygon_t>
	>
static inline std::vector<ring_t> correct(ring_t const &ring, boost::geometry::order_selector order, double remove_spike_min_area = 0.0)
{
	constexpr std::size_t min_nodes = 3;
	if(ring.size() < min_nodes)
		return std::vector<ring_t>();

    std::map<pseudo_vertice_key, pseudo_vertice<point_t>, compare_pseudo_vertice_key> pseudo_vertices;    
    std::set<pseudo_vertice_key, compare_pseudo_vertice_key> start_keys;

	ring_t new_ring = ring;

	// Remove invalid coordinates
	correct_invalid(new_ring);

	// Close ring
	correct_close(new_ring);

	// Correct orientation
	correct_orientation(new_ring, order);

	// Detect self-intersection points (the pseudo vertices only for a ring that has some)
	std::vector<dissolve_crossing<point_t>> crossings;
	if(dissolve_find_crossings<point_t, ring_t>(new_ring, crossings)) {
		if(!crossings.empty())
			dissolve_pseudo_vertices<point_t, ring_t>(new_ring, crossings, pseudo_vertices, start_keys);
	} else
		dissolve_find_intersections(new_ring, pseudo_vertices, start_keys);

	if(start_keys.empty()) {
		if(std::abs(boost::geometry::area(new_ring)) > remove_spike_min_area) {
			std::vector<ring_t> result;
			result.push_back(std::move(new_ring));
			return result;
		} else
			return { };
	}

	return dissolve_generate_rings(pseudo_vertices, start_keys, order, remove_spike_min_area);
}

template<
	typename point_t = boost::geometry::model::d2::point_xy<double>, 
	typename polygon_t = boost::geometry::model::polygon<point_t>,
	typename multi_polygon_t = boost::geometry::model::multi_polygon<polygon_t>
	>
struct combine_non_zero_winding
{
	using box_t = boost::geometry::model::box<point_t>;

	inline void operator()(multi_polygon_t &combined_outers, std::vector<box_t> &outer_boxes, multi_polygon_t &combined_inners, std::vector<box_t> &inner_boxes, polygon_t &poly)
	{
		if(boost::geometry::area(poly) > 0)
			result_combine(combined_outers, outer_boxes, std::move(poly));
		else {
			std::reverse(poly.outer().begin(), poly.outer().end());
			result_combine(combined_inners, inner_boxes, std::move(poly));
		}
	}
};

template<
	typename point_t = boost::geometry::model::d2::point_xy<double>, 
	typename polygon_t = boost::geometry::model::polygon<point_t>,
	typename multi_polygon_t = boost::geometry::model::multi_polygon<polygon_t>
	>
struct combine_odd_even
{
	using box_t = boost::geometry::model::box<point_t>;

	inline void operator()(multi_polygon_t &combined_outers, std::vector<box_t> &, multi_polygon_t &combined_inners, std::vector<box_t> &, polygon_t &poly)
	{
		if(boost::geometry::area(poly) < 0)
			std::reverse(poly.outer().begin(), poly.outer().end());

		multi_polygon_t result;
		boost::geometry::sym_difference(combined_outers, poly, result);
		combined_outers = std::move(result);
	}
};
 
template<
	typename combine_function_t,
	typename point_t = boost::geometry::model::d2::point_xy<double>, 
	typename polygon_t = boost::geometry::model::polygon<point_t>,
	typename ring_t = boost::geometry::model::ring<point_t>,
	typename multi_polygon_t = boost::geometry::model::multi_polygon<polygon_t>
	>
static inline void correct(polygon_t const &input, multi_polygon_t &output, double remove_spike_min_area, combine_function_t combine)
{
	auto order = boost::geometry::point_order<polygon_t>::value;
	auto outer_rings = correct(input.outer(), order, remove_spike_min_area);

	// Calculate all outers and combine them if possible
	multi_polygon_t combined_outers;
	multi_polygon_t combined_inners;
	std::vector<boost::geometry::model::box<point_t>> outer_boxes, inner_boxes;

	for(auto &ring: outer_rings) {
		polygon_t poly;
		poly.outer() = std::move(ring);
		combine(combined_outers, outer_boxes, combined_inners, inner_boxes, poly);
	}

	// Calculate all inners and combine them if possible
	for(auto const &ring: input.inners()) {
		polygon_t poly;
		poly.outer() = std::move(ring);

		multi_polygon_t new_inners;
		correct(poly, new_inners, remove_spike_min_area, combine);

		for(auto &poly: new_inners) {
			result_combine(combined_inners, inner_boxes, std::move(poly));
		}
	}

	// Nothing to cut out of outers without holes: Boost's difference returns each outer as it is
	// (overlay's return_if_one_input_is_empty: select_rings keeps every ring, none within the empty
	// second input; assign_parents returns at once when every ring has a positive area; add_rings
	// copies each ring into a polygon, in input order, if it has 4 points or more and an area that
	// is positive and not 0 by math::equals)
	if(combined_inners.empty() && difference_without_holes(combined_outers, output))
		return;
	if(difference_inside(combined_outers, combined_inners, output))
		return;

	// Cut out all inners from all the outers. Index the rings for the many
	// point-in-ring tests (a hole each), see ring_point_index.h
	{
		RingPointIndexScope pointIndex;
		boost::geometry::difference(combined_outers, combined_inners, output);
	}
}

template<
	typename combine_function_t,
	typename point_t = boost::geometry::model::d2::point_xy<double>, 
	typename polygon_t = boost::geometry::model::polygon<point_t>,
	typename ring_t = boost::geometry::model::ring<point_t>,
	typename multi_polygon_t = boost::geometry::model::multi_polygon<polygon_t>
	>
static inline void correct(multi_polygon_t const &input, multi_polygon_t &output, double remove_spike_min_area, combine_function_t combine)
{
	using box_t = boost::geometry::model::box<point_t>;
	std::vector<box_t> output_boxes;
	for(auto const &polygon: output)
		output_boxes.push_back(boost::geometry::return_envelope<box_t>(polygon));

	for(auto const &polygon: input)
	{
		multi_polygon_t new_polygons;
		correct(polygon, new_polygons, remove_spike_min_area, combine);

		for(auto &new_polygon: new_polygons)
			result_combine(output, output_boxes, std::move(new_polygon));
	}
}

}

template<
	typename point_t = boost::geometry::model::d2::point_xy<double>, 
	typename polygon_t = boost::geometry::model::polygon<point_t>,
	typename multi_polygon_t = boost::geometry::model::multi_polygon<polygon_t>
	>
static inline void correct(polygon_t const &input, multi_polygon_t &output, double remove_spike_min_area = 0.0)
{
	impl::correct(input, output, remove_spike_min_area, impl::combine_non_zero_winding<point_t, polygon_t, multi_polygon_t>());
}

template<
	typename point_t = boost::geometry::model::d2::point_xy<double>, 
	typename polygon_t = boost::geometry::model::polygon<point_t>,
	typename multi_polygon_t = boost::geometry::model::multi_polygon<polygon_t>
	>
static inline void correct_odd_even(polygon_t const &input, multi_polygon_t &output, double remove_spike_min_area = 0.0)
{
	impl::correct(input, output, remove_spike_min_area, impl::combine_odd_even<point_t, polygon_t, multi_polygon_t>());
}


template<
	typename point_t = boost::geometry::model::d2::point_xy<double>, 
	typename polygon_t = boost::geometry::model::polygon<point_t>,
	typename ring_t = boost::geometry::model::ring<point_t>,
	typename multi_polygon_t = boost::geometry::model::multi_polygon<polygon_t>
	>
static inline void correct(multi_polygon_t const &input, multi_polygon_t &output, double remove_spike_min_area = 0.0)
{
	impl::correct(input, output, remove_spike_min_area, impl::combine_non_zero_winding<point_t, polygon_t, multi_polygon_t>());
}

template<
	typename point_t = boost::geometry::model::d2::point_xy<double>, 
	typename polygon_t = boost::geometry::model::polygon<point_t>,
	typename ring_t = boost::geometry::model::ring<point_t>,
	typename multi_polygon_t = boost::geometry::model::multi_polygon<polygon_t>
	>
static inline void correct_odd_even(multi_polygon_t const &input, multi_polygon_t &output, double remove_spike_min_area = 0.0)
{
	impl::correct(input, output, remove_spike_min_area, impl::combine_odd_even<point_t, polygon_t, multi_polygon_t>());
}

}

#endif
