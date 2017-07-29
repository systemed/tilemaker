#ifndef _GEOM_TYPES_H
#define _GEOM_TYPES_H

#include <vector>

// boost::geometry
#include <boost/geometry.hpp>
#include <boost/geometry/algorithms/intersection.hpp>
#include <boost/geometry/geometries/geometries.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/index/rtree.hpp>
typedef boost::geometry::model::d2::point_xy<double> Point; 
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

#ifdef COMPACT_NODES
typedef uint32_t NodeID;
#else
typedef uint64_t NodeID;
#endif
#ifdef COMPACT_WAYS
typedef uint32_t WayID;
#else
typedef uint64_t WayID;
#endif
#define MAX_WAY_ID numeric_limits<WayID>::max()
typedef std::vector<NodeID> NodeVec;
typedef std::vector<WayID> WayVec;

#endif //_GEOM_TYPES_H

