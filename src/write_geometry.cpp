#include "write_geometry.h"
#include <iostream>
#include "helpers.h"

#include <boost/geometry/geometries/segment.hpp>
#include <boost/geometry/index/rtree.hpp>

using namespace std;
namespace geom = boost::geometry;
extern bool verbose;

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
    for(std::size_t i = 0; i < input.size(); ++i) {
		rtree.insert({ input[i], input[i + 1] });    
        nodes[i] = i;
	}
        
    std::priority_queue<std::size_t, std::vector<size_t>> pq;
    for(std::size_t i = 0; i < input.size() - 2; ++i) 
        pq.push(i);      
        
    while(!pq.empty()) {
        auto entry = pq.top();
        pq.pop();
        
        auto start = nodes[entry];
        auto middle = nodes[entry + 1];
        auto end = nodes[entry + 2];                   
                
        simplify_segment line(input[start], input[end]);
        double distance = 0.0;
        for(auto i = start + 1; i < end; ++i) 
            distance = std::max(distance, boost::geometry::distance(line, input[i]));          
    
        if(distance < max_distance) {
            simplify_rtree_counter result;
            boost::geometry::index::query(rtree, boost::geometry::index::intersects(line), std::back_inserter(result));
            boost::geometry::index::query(outer_rtree, boost::geometry::index::intersects(line), std::back_inserter(result));

			std::vector<simplify_segment> nearest;
			constexpr std::size_t nearest_query_size = 5;
			constexpr double nearest_min_distance = 0.0001;
			boost::geometry::index::query(rtree, boost::geometry::index::nearest(line, nearest_query_size), std::back_inserter(nearest));
			boost::geometry::index::query(outer_rtree, boost::geometry::index::nearest(line, nearest_query_size), std::back_inserter(nearest));

			double min_distance = std::numeric_limits<double>::max();
			for(auto const &i: nearest) {
            	double distance = boost::geometry::distance(line, i);
				if(distance > 0.0) 
					min_distance = std::min(min_distance, distance);
			}			

            std::size_t query_expected = ((start == 0 || end == input.size() - 1) ? 2 : 4);
            if(result.size() == query_expected && min_distance > nearest_min_distance) {
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

Polygon simplify(Polygon const &p, double max_distance) 
{
	simplify_rtree outer_rtree;
	for(std::size_t j = 0; j < p.outer().size() - 1; ++j) 
		outer_rtree.insert({ p.outer()[j], p.outer()[j + 1] });    

	std::vector<Ring> combined_inners;
	for(size_t i = 0; i < p.inners().size(); ++i) {
		Ring new_inner = p.inners()[i];
		if(boost::geometry::area(new_inner) < -0.0000001) {
			std::reverse(new_inner.begin(), new_inner.end());
			simplify_combine(combined_inners, std::move(new_inner));
		}
	}

	std::vector<Ring> new_inners;
	for(size_t i = 0; i < combined_inners.size(); ++i) {
		Ring new_inner;
		simplify(combined_inners[i], new_inner, max_distance, outer_rtree);

		if(boost::geometry::area(new_inner) > 0.0000001) {
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
	//if(boost::geometry::area(result.outer()) < 0.0001) {
//		return Polygon();
	//}

	for(auto&& r: new_inners) {
		std::reverse(r.begin(), r.end());
		if(boost::geometry::covered_by(r, result.outer())) {
			result.inners().push_back(r);
		}
	} 

	return result;
}

Linestring simplify(Linestring const &ls, double max_distance) 
{
	Linestring result;
	simplify(ls, result, max_distance);
	return result;
}

MultiPolygon simplify(MultiPolygon const &mp, double max_distance) 
{
	MultiPolygon result_mp;
	for(auto const &p: mp) {
		Polygon new_p = simplify(p, max_distance);
    	if(!new_p.outer().empty()) {
	//		simplify_combine(result_mp, std::move(new_p));
			result_mp.push_back(std::move(new_p));
		}
	}

	return result_mp;
}

WriteGeometryVisitor::WriteGeometryVisitor(const TileBbox *bp, vector_tile::Tile_Feature *fp, double sl) {
	bboxPtr = bp;
	featurePtr = fp;
	simplifyLevel = sl;
}

// Point
void WriteGeometryVisitor::operator()(const Point &p) const {
	if (geom::within(p, bboxPtr->clippingBox)) {
		featurePtr->add_geometry(9);					// moveTo, repeat x1
		pair<int,int> xy = bboxPtr->scaleLatpLon(p.y(), p.x());
		featurePtr->add_geometry((xy.first  << 1) ^ (xy.first  >> 31));
		featurePtr->add_geometry((xy.second << 1) ^ (xy.second >> 31));
		featurePtr->set_type(vector_tile::Tile_GeomType_POINT);
	}
}

// Multipolygon
void WriteGeometryVisitor::operator()(const MultiPolygon &mp) const {
	MultiPolygon current;
	if (simplifyLevel>0) {
		current = simplify(mp, simplifyLevel);
	} else {
		current = mp;
	}

#if BOOST_VERSION >= 105800
	geom::validity_failure_type failure;
	if (verbose && !geom::is_valid(current, failure)) { 
		cout << "output multipolygon has " << boost_validity_error(failure) << endl; 

		if (!geom::is_valid(mp, failure)) 
			cout << "input multipolygon has " << boost_validity_error(failure) << endl; 
		else
			cout << "input multipolygon valid" << endl;
	}
#else	
	if (verbose && !geom::is_valid(current)) { 
		cout << "Output multipolygon is invalid " << endl; 
	}
#endif

	pair<int,int> lastPos(0,0);
	for (MultiPolygon::const_iterator it = current.begin(); it != current.end(); ++it) {
		XYString scaledString;
		Ring ring = geom::exterior_ring(*it);
		for (auto jt = ring.begin(); jt != ring.end(); ++jt) {
			pair<int,int> xy = bboxPtr->scaleLatpLon(jt->get<1>(), jt->get<0>());
			scaledString.push_back(xy);
		}
		bool success = writeDeltaString(&scaledString, featurePtr, &lastPos, true);
		if (!success) continue;

		InteriorRing interiors = geom::interior_rings(*it);
		for (auto ii = interiors.begin(); ii != interiors.end(); ++ii) {
			scaledString.clear();
			XYString scaledInterior;
			for (auto jt = ii->begin(); jt != ii->end(); ++jt) {
				pair<int,int> xy = bboxPtr->scaleLatpLon(jt->get<1>(), jt->get<0>());
				scaledString.push_back(xy);
			}
			writeDeltaString(&scaledString, featurePtr, &lastPos, true);
		}
	}
	featurePtr->set_type(vector_tile::Tile_GeomType_POLYGON);
}

// Multilinestring
void WriteGeometryVisitor::operator()(const MultiLinestring &mls) const {
	MultiLinestring current;
	if (simplifyLevel>0) {
		for(auto const &ls: mls) {
			current.push_back(simplify(ls, simplifyLevel));
		}
	} else {
		current = mls;
	}

	pair<int,int> lastPos(0,0);
	for (MultiLinestring::const_iterator it = current.begin(); it != current.end(); ++it) {
		XYString scaledString;
		for (Linestring::const_iterator jt = it->begin(); jt != it->end(); ++jt) {
			pair<int,int> xy = bboxPtr->scaleLatpLon(jt->get<1>(), jt->get<0>());
			scaledString.push_back(xy);
		}
		writeDeltaString(&scaledString, featurePtr, &lastPos, false);
	}
	featurePtr->set_type(vector_tile::Tile_GeomType_LINESTRING);
}

// Linestring
void WriteGeometryVisitor::operator()(const Linestring &ls) const { 
	Linestring current;
	if (simplifyLevel>0) {
		geom::simplify(ls, current, simplifyLevel);
	} else {
		current = ls;
	}

	pair<int,int> lastPos(0,0);
	XYString scaledString;
	for (Linestring::const_iterator jt = current.begin(); jt != current.end(); ++jt) {
		pair<int,int> xy = bboxPtr->scaleLatpLon(jt->get<1>(), jt->get<0>());
		scaledString.push_back(xy);
	}
	writeDeltaString(&scaledString, featurePtr, &lastPos, false);
	featurePtr->set_type(vector_tile::Tile_GeomType_LINESTRING);
}

// Encode a series of pixel co-ordinates into the feature, using delta and zigzag encoding
bool WriteGeometryVisitor::writeDeltaString(XYString *scaledString, vector_tile::Tile_Feature *featurePtr, pair<int,int> *lastPos, bool closePath) const {
	if (scaledString->size()<2) return false;
	vector<uint32_t> geometry;

	// Start with a moveTo
	int lastX = scaledString->at(0).first;
	int lastY = scaledString->at(0).second;
	int dx = lastX - lastPos->first;
	int dy = lastY - lastPos->second;
	geometry.push_back(9);						// moveTo, repeat x1
	geometry.push_back((dx << 1) ^ (dx >> 31));
	geometry.push_back((dy << 1) ^ (dy >> 31));

	// Then write out the line for each point
	uint len=0;
	geometry.push_back(0);						// this'll be our lineTo opcode, we set it later
	uint end=closePath ? scaledString->size()-1 : scaledString->size();
	for (uint i=1; i<end; i++) {
		int x = scaledString->at(i).first;
		int y = scaledString->at(i).second;
		if (x==lastX && y==lastY) { continue; }
		dx = x-lastX;
		dy = y-lastY;
		geometry.push_back((dx << 1) ^ (dx >> 31));
		geometry.push_back((dy << 1) ^ (dy >> 31));
		lastX = x; lastY = y;
		len++;
	}
	if (closePath && len<2) return false;		// reject ABA polygons
	if (len==0) return false;
	geometry[3] = (len << 3) + 2;				// lineTo plus repeat
	if (closePath) {
		geometry.push_back(7+8);				// closePath
	}
	for (uint i=0; i<geometry.size(); i++) { 
		featurePtr->add_geometry(geometry[i]);
	};
	lastPos->first  = lastX;
	lastPos->second = lastY;
	return true;
}

