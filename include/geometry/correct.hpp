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

#include <vector>
#include <boost/geometry.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/geometries/polygon.hpp>
#include <boost/geometry/geometries/multi_polygon.hpp>
#include <boost/geometry/index/rtree.hpp>
#include <boost/function_output_iterator.hpp>

namespace geometry {

namespace impl {

template<typename C, typename T>
static inline void result_combine(C &result, T &&new_element)
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
			
			std::vector<point_t> output;
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
    while(!start_keys.empty()) {    
		ring_t new_ring;
        
		// Store point in generated polygon
		auto push_point = [&new_ring](auto const &p) { 
            if(new_ring.empty() || boost::geometry::comparable_distance(new_ring.back(), p) > 0)
                new_ring.push_back(p);
		};

		auto is_closed = [](ring_t &ring) {
			if(ring.size() < 2) return false;
			for(std::size_t i = 0; i < ring.size() - 1; ++i) {
				if(boost::geometry::comparable_distance(ring[i], ring.back()) == 0) {
					ring.erase(ring.begin(), ring.begin() + i);
					return true;
				}
			}

			return false;
		};

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

	// Detect self-intersection points
	dissolve_find_intersections(new_ring, pseudo_vertices, start_keys);

	if(start_keys.empty()) {
		if(std::abs(boost::geometry::area(new_ring)) > remove_spike_min_area) 
			return { new_ring };
		else
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
	inline void operator()(multi_polygon_t &combined_outers, multi_polygon_t &combined_inners, polygon_t &poly) 
	{
		if(boost::geometry::area(poly) > 0)
			result_combine(combined_outers, std::move(poly));
		else {
			std::reverse(poly.outer().begin(), poly.outer().end());
			result_combine(combined_inners, std::move(poly));
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
	inline void operator()(multi_polygon_t &combined_outers, multi_polygon_t &combined_inners, polygon_t &poly) 
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

	for(auto &ring: outer_rings) {
		polygon_t poly;
		poly.outer() = std::move(ring);
		combine(combined_outers, combined_inners, poly);
	}

	// Calculate all inners and combine them if possible
	for(auto const &ring: input.inners()) {
		polygon_t poly;
		poly.outer() = std::move(ring);

		multi_polygon_t new_inners;
		correct(poly, new_inners, remove_spike_min_area, combine);

		for(auto &poly: new_inners) {
			result_combine(combined_inners, std::move(poly));
		} 
	}

	// Cut out all inners from all the outers
	boost::geometry::difference(combined_outers, combined_inners, output);
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
	for(auto const &polygon: input)
	{
		multi_polygon_t new_polygons;
		correct(polygon, new_polygons, remove_spike_min_area, combine);

		for(auto &new_polygon: new_polygons) 
			result_combine(output, std::move(new_polygon));
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
