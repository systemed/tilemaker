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

namespace dissolve {

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
	// Generate all by-pass intersections in the graph
	// Generate a list of all by-pass intersections
    for(std::size_t i = 0; i < ring.size(); ++i)
    {
        pseudo_vertices.emplace(pseudo_vertice_key(i, i, 0.0), ring[i]);       
        
        for(std::size_t j = i + 2; j < ring.size() - 1; ++j)
        {
			boost::geometry::model::segment<point_t> line_1(ring[i], ring[i + 1]);
			boost::geometry::model::segment<point_t> line_2(ring[j], ring[j + 1]);
            
            std::vector<point_t> output;
			boost::geometry::intersection(line_1, line_2, output);

			for(auto const &p: output) {
                double scale_1 = boost::geometry::distance(p, ring[i]) / boost::geometry::distance(ring[i + 1], ring[i]);
                double scale_2 = boost::geometry::distance(p, ring[j]) / boost::geometry::distance(ring[j + 1], ring[j]);
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
        }
    }
}

template<
	typename point_t = boost::geometry::model::d2::point_xy<double>, 
	typename ring_t = boost::geometry::model::ring<point_t>
	>
static inline double correct_orientation(ring_t &ring, bool is_inner)
{
	auto area = boost::geometry::area(ring);
	bool should_reverse =
		(!is_inner && area < 0) ||
		(is_inner && area > 0);

	if(should_reverse) {
		std::reverse(ring.begin(), ring.end());
	} 

	return area;
}

template<
	typename point_t = boost::geometry::model::d2::point_xy<double>, 
	typename ring_t = boost::geometry::model::ring<point_t>
	>
static inline std::vector<ring_t> dissolve_generate_rings(
			std::map<pseudo_vertice_key, pseudo_vertice<point_t>, compare_pseudo_vertice_key> &pseudo_vertices,
    		std::set<pseudo_vertice_key, compare_pseudo_vertice_key> &start_keys, 
			bool is_inner = false, double remove_spike_min_area = 0.0)
{
	std::vector<ring_t> result;

	// Generate all polygons by tracing all the intersections
	// Perform union to combine all polygons into single polygon again
    while(!start_keys.empty()) {    
		ring_t new_ring;
        
		// Store point in generated polygon
		auto push_point = [&new_ring](auto const &p) { 
            if(new_ring.empty() || boost::geometry::distance(new_ring.back(), p) > 0)
                new_ring.push_back(p);
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
       	} while(new_ring.size() < 2 || boost::geometry::distance(new_ring.front(), new_ring.back()) > 0);

		auto area = correct_orientation(new_ring, is_inner);

		// Store the point in output polygon
		push_point(i->second.p);

		// Combine with already generated polygons
		if(std::abs(area) > remove_spike_min_area) {
	      	result_combine(result, std::move(new_ring));
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
static inline std::vector<ring_t> dissolve(ring_t const &ring, bool is_inner = false, double remove_spike_min_area = 0.0)
{
    std::map<pseudo_vertice_key, pseudo_vertice<point_t>, compare_pseudo_vertice_key> pseudo_vertices;    
    std::set<pseudo_vertice_key, compare_pseudo_vertice_key> start_keys;
	dissolve_find_intersections(ring, pseudo_vertices, start_keys);
	if(start_keys.empty()) {
		ring_t new_ring = ring;
		correct_orientation(new_ring, is_inner);
		return { new_ring };
	}

	return dissolve_generate_rings(pseudo_vertices, start_keys, is_inner, remove_spike_min_area);
}

template<
	typename point_t = boost::geometry::model::d2::point_xy<double>, 
	typename polygon_t = boost::geometry::model::polygon<point_t>,
	typename ring_t = boost::geometry::model::ring<point_t>,
	typename multi_polygon_t = boost::geometry::model::multi_polygon<polygon_t>
	>
static inline void dissolve(polygon_t const &input, multi_polygon_t &output, double remove_spike_min_area = 0.0)
{
	auto outer_rings = dissolve(input.outer(), false, remove_spike_min_area);
	for(auto const &ring: outer_rings) {
		output.resize(output.size() + 1);
		output.back().outer() = ring;

		for(auto const &i: input.inners()) {
			auto new_rings = dissolve(i, true, remove_spike_min_area);

			for(auto const &new_ring: new_rings) {
				std::vector<ring_t> clipped_rings;
				boost::geometry::intersection(new_ring, output.back().outer(), clipped_rings);

				for(auto &j: clipped_rings)
					result_combine(output.back().inners(), std::move(j));
			}
		}
	}
}

}

#endif
