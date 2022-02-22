#define BOOST_GEOMETRY_NO_ROBUSTNESS
#include "geom.h"

#include <boost/geometry/geometries/segment.hpp>
#include <boost/geometry/index/rtree.hpp>

#include "geometry/correct.hpp"

#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/irange.hpp>

typedef boost::geometry::model::segment<Point> simplify_segment;
typedef boost::geometry::index::rtree<simplify_segment, boost::geometry::index::quadratic<16>> simplify_rtree;


/* -------------------------------------------------------------------------------------
   2-step simplify as per https://github.com/mourner/simplify-js/blob/master/simplify.js
   ------------------------------------------------------------------------------------- */

double getSqDist(Point const &p1, Point const &p2) {
	double dx = p1.x() - p2.x();
	double dy = p1.y() - p2.y();
	return dx*dx + dy*dy;
}
double getSqSegDist(Point const &p, Point const &p1, Point const &p2) {
	double x  = p1.x();
	double y  = p1.y();
	double dx = p2.x() - x;
	double dy = p2.y() - y;
	if (dx != 0 || dy != 0) {
		double t = ((p.x()-x)*dx + (p.y()-y)*dy) / (dx*dx + dy*dy);
		if (t > 1) {
			x = p2.x(); y = p2.y();
		} else if (t > 0) {
			x += dx * t;
			y += dy * t;
		}
	}
	dx = p.x() - x;
	dy = p.y() - y;
	return dx*dx + dy*dy;
}
// basic distance-based simplification
void simplifyRadialDist(Linestring const &points, double sqTolerance, Linestring &newPoints) {
	Point prevPoint = points[0];
	newPoints.clear();
	newPoints.emplace_back(prevPoint);

	Point point;
	for (unsigned i=1; i<points.size(); i++) {
		point = points[i];
		if (getSqDist(point, prevPoint) > sqTolerance) {
			newPoints.emplace_back(point);
			prevPoint = point;
		}
	}
	if (prevPoint.x()!=point.x() && prevPoint.y()!=point.y()) newPoints.emplace_back(point);
}
// simplification using Ramer-Douglas-Peucker algorithm
void simplifyDPStep(Linestring const &points, unsigned first, unsigned last, double sqTolerance, Linestring &simplified) {
	double maxSqDist = sqTolerance;
	unsigned index = 0;
	for (unsigned i = first + 1; i < last; i++) {
		double sqDist = getSqSegDist(points[i], points[first], points[last]);
		if (sqDist > maxSqDist) {
			index = i;
			maxSqDist = sqDist;
		}
	}
	if (maxSqDist > sqTolerance) {
		if (index - first > 1) simplifyDPStep(points, first, index, sqTolerance, simplified);
		simplified.emplace_back(points[index]);
		if (last - index > 1) simplifyDPStep(points, index, last, sqTolerance, simplified);
	}
}
void simplifyDouglasPeucker(Linestring const &points, double sqTolerance, Linestring &simplified) {
    unsigned last = points.size() - 1;
    simplified.clear();
	simplified.emplace_back(points[0]);
    simplifyDPStep(points, 0, last, sqTolerance, simplified);
    simplified.emplace_back(points[last]);
}
// both algorithms combined for awesome performance
Linestring simplifyTwoStep(Linestring const &points, double tolerance) {
    if (points.size() <= 2) { return points; }
    double sqTolerance = tolerance * tolerance;
	Linestring outPoints1; simplifyRadialDist(points, sqTolerance, outPoints1);
	Linestring outPoints2; simplifyDouglasPeucker(outPoints1, sqTolerance, outPoints2);
	return outPoints2;
}

// -------------------------------------------------------------------------------------

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
    if (ls.size() <= 2) { return ls; }
    double sqTolerance = max_distance * max_distance;
	Linestring outPoints1; simplifyRadialDist(ls, sqTolerance, outPoints1);
	Linestring outPoints2; simplifyDouglasPeucker(outPoints1, sqTolerance, outPoints2);
	return outPoints2;
}

MultiPolygon simplify(MultiPolygon const &mp, double max_distance) 
{
	MultiPolygon result_mp;
	for(auto const &p: mp) {
		Polygon new_p = simplify(p, max_distance);
    	if(!new_p.outer().empty()) {
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
