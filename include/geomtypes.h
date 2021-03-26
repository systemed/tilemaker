/*! \file */ 
#ifndef _GEOM_TYPES_H
#define _GEOM_TYPES_H

#ifdef _MSC_VER
using uint = unsigned int;
#endif

#include <vector>
#include <limits>

// boost::geometry
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

#include <boost/interprocess/managed_mapped_file.hpp>
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

// Our geometry types for storing in mmap region
struct mmap {
	using point_t = boost::geometry::model::d2::point_xy<double>;

	template<typename T, typename A> using vector_t = std::vector<T, A>;

    template<typename T> using bi_alloc_t = bi::node_allocator<T, bi::managed_mapped_file::segment_manager>;
    template<typename T> using scoped_alloc_t = boost::container::scoped_allocator_adaptor<T>;

	template<
		typename Point = point_t, 
		template <typename, typename> class Container = vector_t,
		typename Alloc = boost::container::new_allocator<Point>
	>
	struct ring_base_t
		: public Container<Point, Alloc>
	{
		using parent_t = Container<Point, Alloc>;
		using parent_t::parent_t;
	}; 

	using ring_t = ring_base_t<point_t, vector_t, bi_alloc_t<point_t>>;

	template<
		typename Point = point_t, 
		template <typename, typename> class Container = vector_t,
		class Alloc = boost::container::new_allocator<Point>
	>
	struct linestring_base_t
		: public Container<Point, Alloc>
	{
		using parent_t = Container<Point, Alloc>;
		using parent_t::parent_t;
	}; 

	using linestring_t = linestring_base_t<point_t, vector_t, bi_alloc_t<point_t>>;

    struct polygon_data_t
    {
		using inners_type = vector_t<ring_t, scoped_alloc_t<bi_alloc_t<ring_t>>>;
	    using ring_type = ring_t;
    };

    using polygon_parent_t = std::pair<polygon_data_t::ring_type, polygon_data_t::inners_type>;

	struct polygon_t 
		: polygon_parent_t
	{
		using polygon_parent_t::polygon_parent_t;
		using inners_type = polygon_data_t::inners_type;
		using ring_type = polygon_data_t::ring_type;
	};

	using multi_polygon_t = vector_t<mmap::polygon_t, mmap::bi_alloc_t<mmap::polygon_t>>;
};

namespace boost { namespace geometry { namespace traits {  
    template<> struct tag<mmap::polygon_t> { typedef polygon_tag type; }; 
	template<> struct interior_const_type<mmap::polygon_t> 
	{ typedef mmap::polygon_t::inners_type const& type; };

	template<> struct interior_mutable_type<mmap::polygon_t> 
	{ typedef mmap::polygon_t::inners_type& type; };

	template<> struct ring_const_type<mmap::polygon_t> 
	{ typedef mmap::polygon_t::ring_type const& type; };
	template<> struct ring_mutable_type<mmap::polygon_t> 
	{ typedef mmap::polygon_t::ring_type& type; };

	template<> struct exterior_ring<mmap::polygon_t>
	{ 
		static mmap::polygon_t::ring_type& get(mmap::polygon_t& p){
	        return p.first;
    	}

    	static mmap::polygon_t::ring_type const& get(mmap::polygon_t const& p) {
	        return p.first;
    	}
	};	

	template<> struct interior_rings<mmap::polygon_t>
	{
    	static mmap::polygon_t::inners_type& get(mmap::polygon_t& p) {
	        return p.second;
    	}

    	static mmap::polygon_t::inners_type const& get(mmap::polygon_t const& p) {
	        return p.second;
    	}
	};

}}} 

BOOST_GEOMETRY_REGISTER_LINESTRING(mmap::linestring_t)
BOOST_GEOMETRY_REGISTER_RING(mmap::ring_t)
BOOST_GEOMETRY_REGISTER_MULTI_POLYGON(mmap::multi_polygon_t)

typedef uint64_t NodeID;
typedef uint64_t WayID;

#define MAX_WAY_ID std::numeric_limits<WayID>::max()
typedef std::vector<NodeID> NodeVec;
typedef std::vector<WayID> WayVec;

#endif //_GEOM_TYPES_H

