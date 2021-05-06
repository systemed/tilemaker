#include "write_geometry.h"
#include <iostream>
#include "helpers.h"
using namespace std;
namespace geom = boost::geometry;
extern bool verbose;

#include <boost/geometry.hpp>
#include <boost/geometry/geometries/linestring.hpp>
#include <boost/geometry/geometries/point_xy.hpp>

#include <boost/graph/graph_traits.hpp>
#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/dijkstra_shortest_paths.hpp>
#include <boost/property_map/property_map.hpp>

bool dissolve(Ring const &input, Ring &result)
{
  	typedef boost::property<boost::edge_weight_t, double> EdgeWeightProperty;
  	typedef boost::adjacency_list < boost::listS, boost::vecS, boost::directedS, boost::no_property, EdgeWeightProperty > graph_t;
  	typedef boost::graph_traits < graph_t >::vertex_descriptor vertex_descriptor;

  	graph_t g;

  	Ring nodes = input;
  	for(std::size_t i = 0; i < input.size() - 1; ++i) {
    	boost::add_edge(i, i + 1, EdgeWeightProperty(boost::geometry::distance(input[i], input[i + 1])), g);
  	}

    for(std::size_t i = 0; i < input.size() - 1; ++i) {
    	Linestring src({ input[i], input[i + 1] });

    	for(std::size_t j = i + 2; j < input.size() - 1; ++j) {
      		Linestring dst({ input[j], input[j + 1] });

      		std::vector<Point> intersections;
     		boost::geometry::intersection(src, dst, intersections);

			for(auto const &p: intersections) {
				std::size_t v = nodes.size();
				for(std::size_t i = 0; i < nodes.size(); ++i) {
				  	if(boost::geometry::distance(nodes[i], p) == 0) {
						v = i;
						break;
				  	}
				}
				
				if(v == nodes.size())
					nodes.push_back(p);

				if(boost::geometry::distance(nodes[i], nodes[v]) > 0.0)
				  	boost::add_edge(i, v, EdgeWeightProperty(boost::geometry::distance(nodes[i], nodes[v])), g);
				if(boost::geometry::distance(nodes[i+1], nodes[v]) > 0.0)
				  	boost::add_edge(v, i + 1, EdgeWeightProperty(boost::geometry::distance(nodes[i + 1], nodes[v])), g);
		        if(boost::geometry::distance(nodes[j], nodes[v]) > 0.0)
          			boost::add_edge(j, v, EdgeWeightProperty(boost::geometry::distance(nodes[j], nodes[v])), g);
        		if(boost::geometry::distance(nodes[j+1], nodes[v]) > 0.0)
          			boost::add_edge(v, j + 1, EdgeWeightProperty(boost::geometry::distance(nodes[j + 1], nodes[v])), g);
      		}
    	}
  	}

  	std::vector<vertex_descriptor> p(num_vertices(g));
  	std::vector<double> d(num_vertices(g));
  	std::fill(d.begin(), d.end(), std::numeric_limits<double>::max());

  	dijkstra_shortest_paths(g, boost::vertex(0, g),
      predecessor_map(boost::make_iterator_property_map(p.begin(), get(boost::vertex_index, g))).
      distance_map(boost::make_iterator_property_map(d.begin(), get(boost::vertex_index, g))));

  	if(d[num_vertices(g) - 1] == std::numeric_limits<double>::max()) {
    	return false; // No valid path exists
  	}

  	boost::graph_traits < graph_t >::vertex_iterator vbegin, vend;
  	boost::tie(vbegin, vend) = vertices(g);

	for(auto vi = input.size() - 1; vi != 0; )
  	{
		if(result.empty() || boost::geometry::distance(nodes[vi], result.back()) > 0.0)
			boost::geometry::append(result, nodes[vi]);
    	vi = p[vi];
  	}
  	boost::geometry::append(result, nodes[0]);
  	std::reverse(result.begin(), result.end());

  	return true;
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
		// Note that Boost simplify can sometimes produce invalid polygons, resulting in broken coastline etc.
		// In that case, we just revert to the unsimplified version (at the cost of a larger geometry)
		// See comments in https://github.com/boostorg/geometry/pull/460
		// When/if dissolve is merged into Boost.Geometry, we can use that to fix the self-intersections
		geom::simplify(mp, current, simplifyLevel);
	} else {
		current = mp;
	}

	for(auto &p: current) {
		for(auto &i: p.outer()) {
			auto round_i = bboxPtr->floorLatpLon(i.y(), i.x());
			i = Point(round_i.second, round_i.first);
		}

		for(auto &r: p.inners()) {
			Ring inner;

			for(auto &i: r) {
				auto round_i = bboxPtr->floorLatpLon(i.y(), i.x());
				i = Point(round_i.second, round_i.first);
			}
		}
	} 


#if BOOST_VERSION >= 105800
	geom::validity_failure_type failure;
	if (!geom::is_valid(current, failure) && failure != geom::failure_spikes) { 
		if(failure == geom::failure_spikes) {
			geom::remove_spikes(current);
		} else { 
			if(verbose) 
				cout << "Output multipolygon has " << boost_validity_error(failure) << " ";

			for(std::size_t i = 0; i < current.size(); ++i) {
				Ring new_outer;
				dissolve(current[i].outer(), new_outer);
				current[i].outer() = new_outer;

				for(auto &r: current[i].inners()) {
					Ring new_inner;
					dissolve(r, new_inner);
					r = new_inner;
				}

				if(!geom::is_valid(current[i], failure)) {
					if(verbose) {
						std::cout << std::endl << "Dissolve failed " <<  boost_validity_error(failure) << " " << current[i].outer().size() << std::endl;
						for(auto &j: current[i].outer()) {
							std::cout << "xy(" << j.x() << "," << j.y() << "), ";
						}
						std::cout << std::endl;
					}

					return;
				} else {
					if(verbose) 
						std::cout << "dissolve success!! "  << current[i].outer().size() << std::endl;
				}
			}
		}
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
		geom::simplify(mls, current, simplifyLevel);
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

