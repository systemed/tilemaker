#define BOOST_GEOMETRY_NO_ROBUSTNESS
#include "geom.h"

#include <boost/geometry/geometries/segment.hpp>
#include <boost/geometry/index/rtree.hpp>

#include "geometry/correct.hpp"

typedef boost::geometry::model::segment<Point> simplify_segment;
typedef boost::geometry::index::rtree<simplify_segment, boost::geometry::index::quadratic<16>> simplify_rtree;

struct simplify_rtree_counter
{
	using value_type = simplify_segment;
	std::size_t count = 0;
	void push_back(value_type const &) { ++count; }
	std::size_t size() const { return count; }
};

template<typename GeometryType>
void simplify(GeometryType const &input, GeometryType &output, double max_distance, simplify_rtree const &outer_rtree = simplify_rtree())
{        
	simplify_rtree rtree;

    std::deque<std::size_t> nodes(input.size());
    for(std::size_t i = 0; i < input.size(); ++i) 
        nodes[i] = i;
    for(std::size_t i = 0; i < input.size() - 1; ++i)
        rtree.insert({ input[i], input[i + 1] });    
    Box envelope; boost::geometry::envelope(input, envelope);

    std::priority_queue<std::size_t, std::vector<size_t>> pq;
    for(std::size_t i = 0; i < input.size() - 2; ++i) 
        pq.push(i);      
        
    while(!pq.empty()) {
        auto entry = pq.top();
        pq.pop();
        
        auto start = nodes[entry];
        auto middle = nodes[entry + 1];
        auto end = nodes[entry + 2];

        if (input[middle].x()==envelope.min_corner().x() ||
            input[middle].y()==envelope.min_corner().y() ||
            input[middle].x()==envelope.max_corner().x() ||
            input[middle].y()==envelope.max_corner().y()) continue;

        simplify_segment line(input[start], input[end]);
        double distance = 0.0;
        for(auto i = start + 1; i < end; ++i) 
            distance = std::max(distance, boost::geometry::distance(line, input[i]));          
    
        if(boost::geometry::distance(input[start], input[end]) < 2 * max_distance || distance < max_distance) {
            simplify_rtree_counter result;
            boost::geometry::index::query(rtree, boost::geometry::index::intersects(line), std::back_inserter(result));
            boost::geometry::index::query(outer_rtree, boost::geometry::index::intersects(line), std::back_inserter(result));

            std::size_t query_expected = ((start == 0 || end == input.size() - 1) ? 2 : 4);
            if(result.size() == query_expected) {
                nodes.erase(nodes.begin() + entry + 1);
                rtree.remove(simplify_segment(input[start], input[middle]));
                rtree.remove(simplify_segment(input[middle], input[end]));
                rtree.insert(line);
        
                if(entry + 2 < nodes.size()) {
                    pq.push(start);             
                }
            }
        }
    }
    
    for(auto i: nodes)
        boost::geometry::append(output, input[i]);
}

Polygon simplify(Polygon const &p, double max_distance) 
{
	simplify_rtree outer_rtree;
	for(std::size_t j = 0; j < p.outer().size() - 1; ++j) 
		outer_rtree.insert({ p.outer()[j], p.outer()[j + 1] });    

	std::vector<Ring> combined_inners;
	for(size_t i = 0; i < p.inners().size(); ++i) {
		Ring new_inner = p.inners()[i];
		if(boost::geometry::area(new_inner) < 0) {
			std::reverse(new_inner.begin(), new_inner.end());
			simplify_combine(combined_inners, std::move(new_inner));
		}
	}

	std::vector<Ring> new_inners;
	for(size_t i = 0; i < combined_inners.size(); ++i) {
		Ring new_inner;
		simplify(combined_inners[i], new_inner, max_distance, outer_rtree);

		if(boost::geometry::area(new_inner) > max_distance * max_distance) {
			simplify_combine(new_inners, std::move(new_inner));
		}
	}

	simplify_rtree inners_rtree;
	for(auto const &inner: new_inners) {
		for(std::size_t z = 0; z < inner.size() - 1; ++z) 
			inners_rtree.insert({ inner[z], inner[z + 1] });    
	} 

	Polygon result;
	simplify(p.outer(), result.outer(), max_distance, inners_rtree);
	if(boost::geometry::area(result.outer()) < max_distance * max_distance) {
		return Polygon();
	}

	for(auto&& r: new_inners) {
		std::reverse(r.begin(), r.end());
		result.inners().push_back(r);
	} 

	return result;
}

Linestring simplify(Linestring const &ls, double max_distance) 
{
	Linestring result;
	boost::geometry::simplify(ls, result, max_distance);
	return result;
}

MultiPolygon simplify(MultiPolygon const &mp, double max_distance) 
{
	MultiPolygon combined_mp;
	for(auto const &p: mp) {
    	if(!p.outer().empty()) {
			simplify_combine(combined_mp, Polygon(p));
		}
	}

	MultiPolygon result_mp;
	for(auto const &p: combined_mp) {
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
