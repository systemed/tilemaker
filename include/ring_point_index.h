/*! \file */
#ifndef _RING_POINT_INDEX_H
#define _RING_POINT_INDEX_H

// Faster point-in-ring for Boost.Geometry on big rings (included from geom.h).
//
// Boost tests whether a point is in a ring by walking every segment of the ring
// with the cartesian winding strategy. Overlay (difference etc.) and is_valid do
// that once per hole against the outer ring, which is quadratic for a lake with
// 14000 islands and a 100000-point shore line.
//
// The winding strategy only reacts to a segment if the point's x is within the
// segment's x range (math::equals with Boost's epsilon at the ends); every other
// segment leaves the state untouched. So the same answer comes from walking only
// the segments whose x range, widened by far more than that epsilon, holds the
// point's x. The order of the walk doesn't matter: the count is a sum, and a
// point on a segment ends the walk with "on boundary" whenever it is found.
//
// The index is only used inside a RingPointIndexScope, around Boost calls whose
// rings don't change while they run (is_valid, and the ring selection of one
// overlay call). The index refers to a ring by its address; it is dropped when
// the scope ends.
//
// Inside a scope, rings of moderate size also get their bounding box cached: a
// point clearly outside it is outside the ring without walking any segment. That
// helps overlay (union_ of combine_polygons_below), which tests a point of every
// ring without turns against every polygon of the other geometry.

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#include <boost/mpl/vector.hpp>

namespace ring_point_index {

template<typename RingT>
class Index {
public:
	explicit Index(RingT const &ring) : ring(ring), size(ring.size()), data(ring.empty() ? nullptr : &ring[0]) {
		usable = false;
		if (size < 4 || size > std::numeric_limits<uint32_t>::max()) return;
		double minX = std::numeric_limits<double>::max(), maxX = std::numeric_limits<double>::lowest();
		for (auto const &p : ring) {
			double const x = boost::geometry::get<0>(p);
			if (!std::isfinite(x)) return;
			minX = std::min(minX, x); maxX = std::max(maxX, x);
		}
		double const margin = 1e-9 * std::max({ 1.0, std::abs(minX), std::abs(maxX) });
		x0 = minX - margin;
		std::size_t const segments = size - 1;
		bands = std::max<std::size_t>(1, std::min<std::size_t>(segments / 4, 1 << 20));
		double const width = (maxX + margin) - x0;
		if (!(width > 0) || !std::isfinite(width)) return;
		scale = bands / width;
		if (!std::isfinite(scale)) return;

		std::vector<uint32_t> counts(bands + 1, 0);
		auto range = [&](std::size_t i, std::size_t &lo, std::size_t &hi) {
			double const a = boost::geometry::get<0>(ring[i]), b = boost::geometry::get<0>(ring[i + 1]);
			lo = band(std::min(a, b) - margin);
			hi = band(std::max(a, b) + margin);
		};
		std::size_t lo, hi;
		for (std::size_t i = 0; i < segments; i++) {
			range(i, lo, hi);
			if (hi - lo > 64) { wide.push_back(i); continue; }
			for (std::size_t k = lo; k <= hi; k++) counts[k + 1]++;
		}
		for (std::size_t k = 0; k < bands; k++) counts[k + 1] += counts[k];
		start = counts;
		entries.resize(counts[bands]);
		for (std::size_t i = 0; i < segments; i++) {
			range(i, lo, hi);
			if (hi - lo > 64) continue;
			for (std::size_t k = lo; k <= hi; k++) entries[counts[k]++] = i;
		}
		usable = true;
	}

	bool matches(RingT const &r) const {
		return r.size() == size && (size == 0 || &r[0] == data);
	}

	// The same as boost's point_in_range over the whole ring with this winding strategy,
	// or false if the index can't answer (then the caller walks the ring)
	template<typename PointT, typename Strategy>
	bool apply(PointT const &point, Strategy const &strategy, int &result) const {
		if (!usable) return false;
		double const px = boost::geometry::get<0>(point);
		if (!std::isfinite(px)) return false;
		typename Strategy::state_type state;
		bool go = true;
		std::size_t const k = band(px);
		for (uint32_t e = start[k], end = start[k + 1]; go && e < end; e++) {
			uint32_t const i = entries[e];
			go = strategy.apply(point, ring[i], ring[i + 1], state);
		}
		for (std::size_t w = 0; go && w < wide.size(); w++) {
			uint32_t const i = wide[w];
			go = strategy.apply(point, ring[i], ring[i + 1], state);
		}
		result = strategy.result(state);
		return true;
	}

private:
	// Non-decreasing in x, so lo <= x <= hi gives band(lo) <= band(x) <= band(hi)
	std::size_t band(double x) const {
		double const f = std::floor((x - x0) * scale);
		if (!(f > 0)) return 0;
		if (f >= double(bands - 1)) return bands - 1;
		return std::size_t(f);
	}

	RingT const &ring;
	std::size_t size;
	void const *data;
	bool usable;
	double x0 = 0, scale = 0;
	std::size_t bands = 1;
	std::vector<uint32_t> start, entries, wide;
};

// The bounding box of a ring, widened by far more than Boost's epsilon (as the bands
// of the index are). The winding strategy can't count a crossing for a point outside
// it, nor find the point on a segment, so the answer for such a point is "outside".
template<typename RingT>
class RingBox {
public:
	explicit RingBox(RingT const &ring) : size(ring.size()), data(ring.empty() ? nullptr : &ring[0]) {
		usable = false;
		if (ring.empty()) return;
		double x0 = std::numeric_limits<double>::max(), y0 = x0;
		double x1 = std::numeric_limits<double>::lowest(), y1 = x1;
		for (auto const &p : ring) {
			double const x = boost::geometry::get<0>(p), y = boost::geometry::get<1>(p);
			if (!std::isfinite(x) || !std::isfinite(y)) return;
			x0 = std::min(x0, x); x1 = std::max(x1, x);
			y0 = std::min(y0, y); y1 = std::max(y1, y);
		}
		double const margin = 1e-9 * std::max({ 1.0, std::abs(x0), std::abs(x1), std::abs(y0), std::abs(y1) });
		minX = x0 - margin; maxX = x1 + margin;
		minY = y0 - margin; maxY = y1 + margin;
		usable = true;
	}

	bool matches(RingT const &r) const {
		return r.size() == size && (size == 0 || &r[0] == data);
	}

	// NaN compares false, so it is never "clearly outside"
	template<typename PointT>
	bool clearlyOutside(PointT const &point) const {
		if (!usable) return false;
		double const px = boost::geometry::get<0>(point), py = boost::geometry::get<1>(point);
		return px < minX || px > maxX || py < minY || py > maxY;
	}

private:
	std::size_t size;
	void const *data;
	bool usable;
	double minX = 0, minY = 0, maxX = 0, maxY = 0;
};

// Rings with fewer points are walked as before
constexpr std::size_t kMinRingSize = 1024;
// A ring is indexed when it is tested for the third time in a scope
constexpr unsigned kQueriesBeforeIndex = 2;
// Rings with at least this many points get the bounding box test in a scope
constexpr std::size_t kMinBoxRingSize = 32;

template<typename RingT>
struct Cache {
	struct Entry { unsigned queries = 0; std::unique_ptr<Index<RingT>> index; std::unique_ptr<RingBox<RingT>> box; };
	unsigned depth = 0;
	std::unordered_map<RingT const *, Entry> entries;
};

template<typename RingT>
inline Cache<RingT> &cache() {
	static thread_local Cache<RingT> c;
	return c;
}

} // namespace ring_point_index

// Point-in-ring tests on Ring use the index while one of these exists on this thread
class RingPointIndexScope {
public:
	RingPointIndexScope() { ring_point_index::cache<Ring>().depth++; }
	~RingPointIndexScope() {
		auto &c = ring_point_index::cache<Ring>();
		if (--c.depth == 0) c.entries.clear();
	}
	RingPointIndexScope(RingPointIndexScope const &) = delete;
	RingPointIndexScope &operator=(RingPointIndexScope const &) = delete;
};

namespace ring_point_index {

// Boost >= 1.77 or so passes an umbrella strategy to point_in_geometry and takes the
// point/ring strategy from strategy.relate(point, ring); older Boost (1.74) passes
// the point-in-ring strategy itself
template<typename S, typename P, typename R, typename = void>
struct HasRelate : std::false_type {};
template<typename S, typename P, typename R>
struct HasRelate<S, P, R, std::void_t<decltype(std::declval<S const &>().relate(std::declval<P const &>(), std::declval<R const &>()))>>
	: std::true_type {};

template<typename S, typename P, typename R>
inline auto pointInRingStrategy(S const &strategy, P const &point, R const &ring) {
	if constexpr (HasRelate<S, P, R>::value) return strategy.relate(point, ring);
	else return strategy;
}

// The index reproduces cartesian_winding with the default calculation type only
// (its point type arguments are unused, kept by Boost for backward compatibility)
template<typename T> struct IsCartesianWinding : std::false_type {};
template<typename P1, typename P2>
struct IsCartesianWinding<boost::geometry::strategy::within::cartesian_winding<P1, P2, void>> : std::true_type {};

} // namespace ring_point_index

namespace boost { namespace geometry { namespace detail_dispatch { namespace within {

// Same as boost's point_in_geometry<Ring, ring_tag>, but the walk over the ring's
// segments may use the index (see above). Ring is closed and clockwise, so the
// normalized (closed, clockwise) view boost walks is the ring itself.
template <>
struct point_in_geometry<Ring, ring_tag>
{
	static_assert(geometry::closure<Ring>::value == closed && geometry::point_order<Ring>::value == clockwise,
	              "the ring is walked as it is");

	template <typename Point_, typename Strategy>
	static inline int apply(Point_ const& point, Ring const& ring, Strategy const& strategy)
	{
		if ( boost::size(ring) < core_detail::closure::minimum_ring_size
		                            <
		                                geometry::closure<Ring>::value
		                            >::value )
		{
			return -1;
		}

		auto const pir = ring_point_index::pointInRingStrategy(strategy, point, ring);
		using pir_t = std::decay_t<decltype(pir)>;
		if constexpr (ring_point_index::IsCartesianWinding<pir_t>::value) {
			if (ring.size() >= ring_point_index::kMinBoxRingSize) {
				auto &c = ring_point_index::cache<Ring>();
				if (c.depth > 0) {
					auto &entry = c.entries[&ring];
					if (!entry.box || !entry.box->matches(ring))
						entry.box = std::make_unique<ring_point_index::RingBox<Ring>>(ring);
					if (entry.box->clearlyOutside(point)) return -1;
					if (ring.size() >= ring_point_index::kMinRingSize) {
						if (entry.index && !entry.index->matches(ring)) { entry.index.reset(); entry.queries = 0; }
						if (!entry.index && ++entry.queries > ring_point_index::kQueriesBeforeIndex)
							entry.index = std::make_unique<ring_point_index::Index<Ring>>(ring);
						int result;
						if (entry.index && entry.index->apply(point, pir, result)) return result;
					}
				}
			}
		}

		return detail::within::point_in_range(point, ring, pir);
	}
};

}}}} // namespace boost::geometry::detail_dispatch::within

// is_valid tests the holes of a polygon for overlaps with boost's partition, which
// hands its visitor every pair of holes in a cell, also pairs whose boxes are far
// apart. The visitor asks relate() whether their interiors or boundaries meet; for
// a big hole (an island with 15000 points) relate sectionalizes it again for each
// of hundreds of small holes. Two rings whose boxes are clearly apart can't meet:
// relate finds no turns (section boxes only grow by epsilon) and the point-in-ring
// tests put each ring outside the other, so none of the asked-for cells is set and
// the answer is false. Answer that without the work.
namespace ring_point_index {

inline bool boxesClearlyApart(Ring const &a, Ring const &b) {
	if (a.empty() || b.empty()) return false;
	auto ba = boost::geometry::return_envelope<Box>(a), bb = boost::geometry::return_envelope<Box>(b);
	double const c[8] = { ba.min_corner().x(), ba.min_corner().y(), ba.max_corner().x(), ba.max_corner().y(),
	                      bb.min_corner().x(), bb.min_corner().y(), bb.max_corner().x(), bb.max_corner().y() };
	double m = 1;
	for (double v : c) { if (!std::isfinite(v)) return false; m = std::max(m, std::abs(v)); }
	double const gap = 1e-9 * m;
	return ba.max_corner().x() + gap < bb.min_corner().x() || bb.max_corner().x() + gap < ba.min_corner().x()
	    || ba.max_corner().y() + gap < bb.min_corner().y() || bb.max_corner().y() + gap < ba.min_corner().y();
}

} // namespace ring_point_index

// util::type_sequence doesn't exist in Boost 1.74; declaring it is harmless there
// (it is never used then) and a redeclaration where boost defines it
namespace boost { namespace geometry { namespace util { template <typename ...Ts> struct type_sequence; } } }
namespace ring_point_index {

// The handler of is_valid_polygon's nested-holes test: true if II, IB or BI is set.
// Boost 1.74 lists the masks in an mpl::vector, newer Boost in a util::type_sequence.
template<template<typename...> class Seq>
using HolesOverlapHandlerT = boost::geometry::detail::relate::static_mask_handler<
	Seq<boost::geometry::de9im::static_mask<'T'>,
	    boost::geometry::de9im::static_mask<'*', 'T'>,
	    boost::geometry::de9im::static_mask<'*', '*', '*', 'T'>>,
	true>;

template<typename Result>
constexpr bool isHolesOverlapHandler() {
	return std::is_same<Result, HolesOverlapHandlerT<boost::geometry::util::type_sequence>>::value
	    || std::is_same<Result, HolesOverlapHandlerT<boost::mpl::vector>>::value;
}

} // namespace ring_point_index

namespace boost { namespace geometry { namespace dispatch {

template <>
struct relate<Ring, Ring, ring_tag, ring_tag, 2, 2, true>
	: detail::relate::areal_areal<Ring, Ring>
{
	template <typename Result, typename Strategy>
	static inline void apply(Ring const &ring1, Ring const &ring2, Result &result, Strategy const &strategy)
	{
		// The handler starts with every cell 'F': result() is false, as relate would give
		if (ring_point_index::isHolesOverlapHandler<Result>()
		    && ring_point_index::boxesClearlyApart(ring1, ring2))
			return;
		detail::relate::areal_areal<Ring, Ring>::apply(ring1, ring2, result, strategy);
	}
};

}}} // namespace boost::geometry::dispatch

#endif
