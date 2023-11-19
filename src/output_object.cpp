/*
	OutputObject - any object (node, linestring, polygon) to be outputted to tiles
*/

#include "output_object.h"
#include "helpers.h"
#include "coordinates_geom.h"
#include <iostream>
using namespace std;
namespace geom = boost::geometry;

// **********************************************************

std::ostream& operator<<(std::ostream& os, OutputGeometryType geomType)
{
	switch(geomType) {
		case POINT_:
			os << "POINT";
			break;
		case LINESTRING_:
			os << "LINESTRING";
			break;
		case MULTILINESTRING_:
			os << "MULTILINESTRING";
			break;
		case POLYGON_:
			os << "POLYGON";
			break;
	}

	return os;
}


// Write attribute key/value pairs (dictionary-encoded)
void OutputObject::writeAttributes(
	vector<string> *keyList, 
	vector<vector_tile::Tile_Value> *valueList, 
	vector_tile::Tile_Feature *featurePtr,
	char zoom) const {

	for(auto const &it: attributes->values) {
		if (it.minzoom > zoom) continue;

		// Look for key
		std::string const &key = it.key;
		auto kt = find(keyList->begin(), keyList->end(), key);
		if (kt != keyList->end()) {
			uint32_t subscript = kt - keyList->begin();
			featurePtr->add_tags(subscript);
		} else {
			uint32_t subscript = keyList->size();
			keyList->push_back(key);
			featurePtr->add_tags(subscript);
		}
		
		// Look for value
		vector_tile::Tile_Value const &value = it.value;
		int subscript = findValue(valueList, value);
		if (subscript>-1) {
			featurePtr->add_tags(subscript);
		} else {
			uint32_t subscript = valueList->size();
			valueList->push_back(value);
			featurePtr->add_tags(subscript);
		}

		//if(value.has_string_value())
		//	std::cout << "Write attr: " << key << " " << value.string_value() << std::endl;	
	}
}

Geometry buildWayGeometry(OSMStore &osmStore, OutputObject const &oo, const TileBbox &bbox) 
{
	switch(oo.geomType) {
		case POINT_:
		{
			auto p = osmStore.retrieve_point((oo.objectID >> OSMID_TYPE_OFFSET) > 0 ? osmStore.osm() : osmStore.shp(), oo.objectID);
			if (geom::within(p, bbox.clippingBox)) {
				return p;
			} 
			return MultiLinestring();
		}

		case LINESTRING_:
		{
			auto const &ls = osmStore.retrieve_linestring((oo.objectID >> OSMID_TYPE_OFFSET) > 0 ? osmStore.osm() : osmStore.shp(), oo.objectID);

			MultiLinestring out;
			if(ls.empty())
				return out;

			Linestring current_ls;
			geom::append(current_ls, ls[0]);

			for(size_t i = 1; i < ls.size(); ++i) {
				if(!geom::intersects(Linestring({ ls[i-1], ls[i] }), bbox.clippingBox)) {
					if(current_ls.size() > 1)
						out.push_back(std::move(current_ls));
					current_ls.clear();
				}
				geom::append(current_ls, ls[i]);
			}

			if(current_ls.size() > 1)
				out.push_back(std::move(current_ls));

			MultiLinestring result;
			geom::intersection(out, bbox.getExtendBox(), result);
			return result;
		}

		case MULTILINESTRING_:
		{
			auto const &mls = osmStore.retrieve_multi_linestring((oo.objectID >> OSMID_TYPE_OFFSET) > 0 ? osmStore.osm() : osmStore.shp(), oo.objectID);
			// investigate whether filtering the constituent linestrings improves performance
			MultiLinestring result;
			geom::intersection(mls, bbox.getExtendBox(), result);
			return result;
		}

		case POLYGON_:
		{
			auto const &input = osmStore.retrieve_multi_polygon((oo.objectID >> OSMID_TYPE_OFFSET) > 0 ? osmStore.osm() : osmStore.shp(), oo.objectID);

			Box box = bbox.clippingBox;
			
			if (bbox.endZoom) {
				for(auto const &p: input) {
					for(auto const &inner: p.inners()) {
						for(std::size_t i = 0; i < inner.size() - 1; ++i) 
						{
							Point p1 = inner[i];
							Point p2 = inner[i + 1];

							if(geom::within(p1, bbox.clippingBox) != geom::within(p2, bbox.clippingBox)) {
								box.min_corner() = Point(	
									std::min(box.min_corner().x(), std::min(p1.x(), p2.x())), 
									std::min(box.min_corner().y(), std::min(p1.y(), p2.y())));
								box.max_corner() = Point(	
									std::max(box.max_corner().x(), std::max(p1.x(), p2.x())), 
									std::max(box.max_corner().y(), std::max(p1.y(), p2.y())));
							}
						}
					}

					for(std::size_t i = 0; i < p.outer().size() - 1; ++i) {
						Point p1 = p.outer()[i];
						Point p2 = p.outer()[i + 1];

						if(geom::within(p1, bbox.clippingBox) != geom::within(p2, bbox.clippingBox)) {
							box.min_corner() = Point(	
								std::min(box.min_corner().x(), std::min(p1.x(), p2.x())), 
								std::min(box.min_corner().y(), std::min(p1.y(), p2.y())));
							box.max_corner() = Point(	
								std::max(box.max_corner().x(), std::max(p1.x(), p2.x())), 
								std::max(box.max_corner().y(), std::max(p1.y(), p2.y())));
						}
					}
				}

				Box extBox = bbox.getExtendBox();
				box.min_corner() = Point(	
					std::max(box.min_corner().x(), extBox.min_corner().x()), 
					std::max(box.min_corner().y(), extBox.min_corner().y()));
				box.max_corner() = Point(	
					std::min(box.max_corner().x(), extBox.max_corner().x()), 
					std::min(box.max_corner().y(), extBox.max_corner().y()));
			}

			MultiPolygon mp;
			geom::assign(mp, input);
			fast_clip(mp, box);
			geom::correct(mp);
			if (!geom::is_valid(mp)) make_valid(mp);
			return mp;
		}

		default:
			throw std::runtime_error("Invalid output geometry");
	}
}

LatpLon buildNodeGeometry(OSMStore &osmStore, OutputObject const &oo, const TileBbox &bbox)
{
	switch(oo.geomType) {
		case POINT_:
		{
			auto p = osmStore.retrieve_point((oo.objectID >> OSMID_TYPE_OFFSET) > 0 ? osmStore.osm() : osmStore.shp(), oo.objectID);
			LatpLon out;
			out.latp = p.y();
			out.lon = p.x();
		 	return out;
		}

		default:
			break;
	}

	throw std::runtime_error("Geometry type is not point");			
}

// Find a value in the value dictionary
// (we can't easily use find() because of the different value-type encoding - 
//	should be possible to improve this though)
int OutputObject::findValue(vector<vector_tile::Tile_Value> *valueList, vector_tile::Tile_Value const &value) const {
	for (size_t i=0; i<valueList->size(); i++) {
		vector_tile::Tile_Value v = valueList->at(i);
		if (v.has_string_value() && value.has_string_value() && v.string_value()==value.string_value()) { return i; }
		if (v.has_float_value()  && value.has_float_value()  && v.float_value() ==value.float_value() ) { return i; }
		if (v.has_bool_value()	 && value.has_bool_value()   && v.bool_value()  ==value.bool_value()	) { return i; }
	}
	return -1;
}

// Comparision functions

bool operator==(const OutputObjectRef x, const OutputObjectRef y) {
	return
		x->layer == y->layer &&
		x->z_order == y->z_order &&
		x->geomType == y->geomType &&
		x->attributes == y->attributes &&
		x->objectID == y->objectID;
} 

namespace vector_tile {
	bool operator==(const vector_tile::Tile_Value &x, const vector_tile::Tile_Value &y) {
		std::string strx = x.SerializeAsString();
		std::string stry = y.SerializeAsString();
		return strx == stry;
	}
	bool operator<(const vector_tile::Tile_Value &x, const vector_tile::Tile_Value &y) {
		std::string strx = x.SerializeAsString();
		std::string stry = y.SerializeAsString();
		return strx < stry;
	}
}
