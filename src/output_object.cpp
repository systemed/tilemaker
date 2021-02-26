/*
	OutputObject - any object (node, linestring, polygon) to be outputted to tiles
*/

#include "output_object.h"
#include "helpers.h"
#include <iostream>
using namespace std;
using namespace ClipperLib;
namespace geom = boost::geometry;

// **********************************************************


// Write attribute key/value pairs (dictionary-encoded)
void OutputObject::writeAttributes(
	vector<string> *keyList, 
	vector<vector_tile::Tile_Value> *valueList, 
	vector_tile::Tile_Feature *featurePtr) const {

	for(auto const &it: attributes->entries) {
		// Look for key
		std::string const &key = it->first;
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
		vector_tile::Tile_Value const &value = it->second; 
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
		case POINT:
		case CACHED_POINT:
		case CENTROID:
		{
			auto const &p = osmStore.retrieve<mmap::point_t>(oo.handle);
			if (geom::within(p, bbox.clippingBox)) {
				return p;
			} 
			return MultiLinestring();
		}

		case LINESTRING:
		case CACHED_LINESTRING:
		{
			MultiLinestring out;
			geom::intersection(osmStore.retrieve<mmap::linestring_t>(oo.handle), bbox.clippingBox, out);
			return out;
		}

		case POLYGON:
		case CACHED_POLYGON:
		{
			auto const &mp = osmStore.retrieve<mmap::multi_polygon_t>(oo.handle);

			Polygon clippingPolygon;

			geom::convert(bbox.clippingBox, clippingPolygon);
			if (!geom::intersects(mp, clippingPolygon)) { return MultiPolygon(); }
			if (geom::within(mp, clippingPolygon)) { 
				MultiPolygon out;
				boost::geometry::assign(out, mp);
				return out; 
			}

			try {
				MultiPolygon out;
				geom::intersection(mp, clippingPolygon, out);
				return out;
			} catch (geom::overlay_invalid_input_exception &err) {
				std::cout << "Couldn't clip polygon (self-intersection)" << std::endl;
				return MultiPolygon(); // blank
			}
		}

		default:
			throw std::runtime_error("Invalid output geometry");
	}
}

LatpLon buildNodeGeometry(OSMStore &osmStore, OutputObject const &oo, const TileBbox &bbox)
{
	switch(oo.geomType) {
		case POINT:
		case CACHED_POINT:
		case CENTROID:
		{
			auto const &pt = osmStore.retrieve<mmap::point_t>(oo.handle);
			LatpLon out;
			out.latp = pt.y();
			out.lon = pt.x();
		 	return out;
		}

		default:	
			throw std::runtime_error("Geometry type is not point");

	}
}

bool intersects(OSMStore &osmStore, OutputObject const &oo, Point const &p)
{
	switch(oo.geomType) {
		case POINT:
		case CACHED_POINT:
		case CENTROID:
			return boost::geometry::intersects(osmStore.retrieve<mmap::point_t>(oo.handle), p);

		case LINESTRING:
		case CACHED_LINESTRING:
			return boost::geometry::intersects(osmStore.retrieve<mmap::linestring_t>(oo.handle), p);


		case POLYGON:
		case CACHED_POLYGON:
			return boost::geometry::intersects(osmStore.retrieve<mmap::multi_polygon_t>(oo.handle), p);

		default:
			throw std::runtime_error("Invalid output geometry");
	}
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

bool operator==(const OutputObjectRef &x, const OutputObjectRef &y) {
	return
		x->layer == y->layer &&
		x->geomType == y->geomType &&
		x->attributes->id == y->attributes->id &&
		x->objectID == y->objectID;
}

// Do lexicographic comparison, with the order of: layer, geomType, attributes, and objectID.
// Note that attributes is preffered to objectID.
// It is to arrange objects with the identical attributes continuously.
// Such objects will be merged into one object, to reduce the size of output.
bool operator<(const OutputObjectRef &x, const OutputObjectRef &y) {
	if (x->layer < y->layer) return true;
	if (x->layer > y->layer) return false;
	if (x->geomType < y->geomType) return true;
	if (x->geomType > y->geomType) return false;
	if (x->attributes->id < y->attributes->id) return true;
	if (x->attributes->id > y->attributes->id) return false;
	if (x->objectID < y->objectID) return true;
	return false;
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
