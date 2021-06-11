/*! \file */ 
#ifndef _GEOM_TYPES_H
#define _GEOM_TYPES_H

#ifdef _MSC_VER
using uint = unsigned int;
#endif

#include <vector>
#include <limits>

// boost::geometry
#define BOOST_GEOMETRY_INCLUDE_SELF_TURNS
#include <boost/geometry.hpp>
#include <boost/geometry/algorithms/intersection.hpp>
#include <boost/geometry/geometries/geometries.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/index/rtree.hpp>
#include <boost/geometry/geometries/register/linestring.hpp>
#include <boost/geometry/geometries/register/ring.hpp>
#include <boost/geometry/geometries/register/multi_linestring.hpp>
#include <boost/geometry/geometries/register/multi_polygon.hpp>
#include <boost/container/scoped_allocator.hpp>

#include <boost/interprocess/managed_external_buffer.hpp>
#include <boost/interprocess/allocators/node_allocator.hpp>

namespace bi = boost::interprocess;

typedef boost::geometry::model::d2::point_xy<double> Point; 
typedef boost::geometry::model::point<double, 2, boost::geometry::cs::spherical_equatorial<boost::geometry::degree> > DegPoint;
typedef boost::geometry::model::linestring<Point> Linestring;
typedef boost::geometry::model::polygon<Point> Polygon;
typedef boost::geometry::model::multi_polygon<Polygon> MultiPolygon;
typedef boost::geometry::model::multi_linestring<Linestring> MultiLinestring;
typedef boost::geometry::model::box<Point> Box;
typedef boost::geometry::ring_type<Polygon>::type Ring;
typedef boost::geometry::interior_type<Polygon>::type InteriorRing;
typedef boost::variant<Point,Linestring,MultiLinestring,MultiPolygon> Geometry;
typedef std::pair<Box, uint> IndexValue;
typedef boost::geometry::index::rtree< IndexValue, boost::geometry::index::quadratic<16> > RTree;

typedef uint64_t NodeID;
typedef uint64_t WayID;

#define MAX_WAY_ID std::numeric_limits<WayID>::max()
typedef std::vector<NodeID> NodeVec;
typedef std::vector<WayID> WayVec;

// Perform self-intersection aware simplification of geometry types
Linestring simplify(Linestring const &ls, double max_distance);
Polygon simplify(Polygon const &p, double max_distance);
MultiPolygon simplify(MultiPolygon const &mp, double max_distance);

// Combine overlapping elements by performing a union
template<typename C, typename T>
void simplify_combine(C &result, T &&new_element)
{
    result.push_back(new_element);

   	for(std::size_t i = 0; i < result.size() - 1; ) {
        if(!boost::geometry::intersects(result[i], result.back())) {
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
       	result.erase(result.begin() + i);
    } 
}

namespace geom = boost::geometry;

template<class GeometryT>
void make_valid(GeometryT &geom) { }

void make_valid(MultiPolygon &mp);

#endif //_GEOM_TYPES_H

