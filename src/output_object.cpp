/*
	OutputObject - any object (node, linestring, polygon) to be outputted to tiles

	Possible future improvements to save memory:
	- use a global dictionary for attribute key/values
*/

#include "output_object.h"
#include "helpers.h"
#include <iostream>
using namespace std;
using namespace ClipperLib;
namespace geom = boost::geometry;

ClipGeometryVisitor::ClipGeometryVisitor(const Box &cbox) : clippingBox(cbox) {
	const Point &minc = clippingBox.min_corner();
	const Point &maxc = clippingBox.max_corner();
	clippingPath.push_back(IntPoint(std::round(minc.x() * CLIPPER_SCALE), std::round(minc.y() * CLIPPER_SCALE)));	
	clippingPath.push_back(IntPoint(std::round(maxc.x() * CLIPPER_SCALE), std::round(minc.y() * CLIPPER_SCALE)));	
	clippingPath.push_back(IntPoint(std::round(maxc.x() * CLIPPER_SCALE), std::round(maxc.y() * CLIPPER_SCALE)));	
	clippingPath.push_back(IntPoint(std::round(minc.x() * CLIPPER_SCALE), std::round(maxc.y() * CLIPPER_SCALE)));	
}

Geometry ClipGeometryVisitor::operator()(const Point &p) const {
	if (geom::within(p, clippingBox)) {
		return p;
	} else {
		return MultiLinestring(); // return a blank geometry
	}
}

Geometry ClipGeometryVisitor::operator()(const Linestring &ls) const {
	MultiLinestring out;
	geom::intersection(ls, clippingBox, out);
	return out;
}

Geometry ClipGeometryVisitor::operator()(const MultiLinestring &mls) const {
#if BOOST_VERSION <= 105800
	// Due to https://svn.boost.org/trac/boost/ticket/11268, we can't clip a MultiLinestring with Boost 1.56-1.58
	return mls;
#else
	MultiLinestring out;
	geom::intersection(mls, clippingBox, out);
	return out;
#endif
}

Geometry ClipGeometryVisitor::operator()(const MultiPolygon &mp) const {
	Polygon clippingPolygon;
	geom::convert(clippingBox,clippingPolygon);
	if (geom::within(mp, clippingPolygon)) { return mp; }

	MultiPolygon out;
	geom::intersection(mp,clippingBox,out);
	return out;
}

// **********************************************************

OutputObject::OutputObject(OutputGeometryType type, uint_least8_t l, NodeID id) {
	geomType = type;
	layer = l;
	objectID = id;
}

OutputObject::~OutputObject() { }

void OutputObject::addAttribute(const string &key, vector_tile::Tile_Value &value) {
	attributes[key]=value;
}

bool OutputObject::hasAttribute(const string &key) const {
	auto it = attributes.find(key);
	return it != attributes.end();
}

// Write attribute key/value pairs (dictionary-encoded)
void OutputObject::writeAttributes(vector<string> *keyList, vector<vector_tile::Tile_Value> *valueList, vector_tile::Tile_Feature *featurePtr) const {
	for (auto it = attributes.begin(); it != attributes.end(); ++it) {

		// Look for key
		string key = it->first;
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
		vector_tile::Tile_Value value = it->second;
		int subscript = findValue(valueList, &value);
		if (subscript>-1) {
			featurePtr->add_tags(subscript);
		} else {
			uint32_t subscript = valueList->size();
			valueList->push_back(value);
			featurePtr->add_tags(subscript);
		}
	}
}

// Find a value in the value dictionary
// (we can't easily use find() because of the different value-type encoding - 
//  should be possible to improve this though)
int OutputObject::findValue(vector<vector_tile::Tile_Value> *valueList, vector_tile::Tile_Value *value) const {
	for (size_t i=0; i<valueList->size(); i++) {
		vector_tile::Tile_Value v = valueList->at(i);
		if (v.has_string_value() && value->has_string_value() && v.string_value()==value->string_value()) { return i; }
		if (v.has_float_value()  && value->has_float_value()  && v.float_value() ==value->float_value() ) { return i; }
		if (v.has_bool_value()   && value->has_bool_value()   && v.bool_value()  ==value->bool_value()  ) { return i; }
	}
	return -1;
}

// Comparision functions

bool operator==(const OutputObjectRef &x, const OutputObjectRef &y) {
	return
		x->layer == y->layer &&
		x->geomType == y->geomType &&
		x->attributes == y->attributes &&
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
	if (x->attributes < y->attributes) return true;
	if (x->attributes > y->attributes) return false;
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

// ***********************************************

OutputObjectOsmStore::OutputObjectOsmStore(OutputGeometryType type, uint_least8_t l, NodeID id,
	Geometry geom):
	OutputObject(type, l, id),
	geom(geom) { }

OutputObjectOsmStore::~OutputObjectOsmStore() { }

Geometry OutputObjectOsmStore::buildWayGeometry(const TileBbox &bbox) const {
	ClipGeometryVisitor clip(bbox.clippingBox);
	return boost::apply_visitor(clip, geom);	
}

// Add a node geometry
LatpLon OutputObjectOsmStore::buildNodeGeometry(const TileBbox &bbox) const {
	const Point *pt = boost::get<Point>(&geom);
	if(pt == nullptr) throw runtime_error("Geometry type is not point");
	LatpLon out;
	out.latp = pt->y();
	out.lon = pt->x();
	return out;
}

// **********************************************

OutputObjectCached::OutputObjectCached(OutputGeometryType type, uint_least8_t l, NodeID id, 
	const std::vector<Geometry> &cachedGeometries):
	OutputObject(type, l, id),
	cachedGeometries(cachedGeometries) { }

OutputObjectCached::~OutputObjectCached() { }

Geometry OutputObjectCached::buildWayGeometry(const TileBbox &bbox) const {
	ClipGeometryVisitor clip(bbox.clippingBox);

	try {
		if (geomType==CACHED_LINESTRING || geomType==CACHED_POLYGON || geomType==CACHED_POINT) {
			const Geometry &g = this->cachedGeometries[objectID];
			return boost::apply_visitor(clip, g);
		}
	} catch (std::invalid_argument &err) {
		cerr << "Error in buildWayGeometry: " << err.what() << endl;
	}

	return MultiLinestring(); // return a blank geometry
}

LatpLon OutputObjectCached::buildNodeGeometry(const TileBbox &bbox) const {
	throw runtime_error("Geometry point type not supported");
	LatpLon out;
	out.latp = 0;
	out.lon = 0;	
	return out;
}

