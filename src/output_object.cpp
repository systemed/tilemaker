/*
	OutputObject - any object (node, linestring, polygon) to be outputted to tiles

	Possible future improvements to save memory:
	- use a global dictionary for attribute key/values
*/

#include "output_object.h"
#include "helpers.h"
using namespace std;
using namespace ClipperLib;
namespace geom = boost::geometry;

ClipGeometryVisitor::ClipGeometryVisitor(const Box &cbox) : clippingBox(cbox) 
{
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
	string reason;

	// Convert boost geometries to clipper paths 
	Paths simplified;
	ConvertToClipper(mp, simplified);

	// Clip to box
	Paths clipped;
	Clipper c2;
	c2.StrictlySimple(true);
	c2.AddPaths(simplified, ptSubject, true);
	c2.AddPath(this->clippingPath, ptClip, true);
	c2.Execute(ctIntersection, clipped, pftEvenOdd, pftEvenOdd);

	// Convert back to boost geometries
	ConvertFromClipper(clipped, out);

	return out;
}

// **********************************************************

OutputObject::OutputObject(OutputGeometryType type, uint_least8_t l, NodeID id) {
	geomType = type;
	layer = l;
	objectID = id;
}

void OutputObject::addAttribute(const string &key, vector_tile::Tile_Value &value) {
	attributes[key]=value;
}

bool OutputObject::hasAttribute(const string &key) const {
	auto it = attributes.find(key);
	return it != attributes.end();
}

// Assemble a linestring or polygon into a Boost geometry, and clip to bounding box
// Returns a boost::variant -
//   POLYGON->MultiPolygon, CENTROID->Point, LINESTRING->MultiLinestring
Geometry OutputObject::buildWayGeometry(const OSMStore &osmStore,
                      TileBbox *bboxPtr, 
                      const vector<Geometry> &cachedGeometries) const {

	ClipGeometryVisitor clip(bboxPtr->clippingBox);

	try {
		if (geomType==POLYGON || geomType==CENTROID) {
			// polygon
			MultiPolygon mp;
			if (osmStore.ways.count(objectID)) {
				mp.emplace_back(osmStore.nodeListPolygon(objectID));
			} else {
				mp = osmStore.wayListMultiPolygon(objectID);
			}

			// write out
			if (geomType==CENTROID) {
				// centroid only
				Point p;
				geom::centroid(mp, p);
				return clip(p);

			} else {
				// full polygon
				return clip(mp);
			}

		} else if (geomType==LINESTRING) {
			// linestring
			Linestring ls;
			if (osmStore.ways.count(objectID)) {
				ls = osmStore.nodeListLinestring(objectID);
			}
			return clip(ls);

		} else if (geomType==CACHED_LINESTRING || geomType==CACHED_POLYGON || geomType==CACHED_POINT) {
			const Geometry &g = cachedGeometries[objectID];
			return boost::apply_visitor(clip, g);
		}
	} catch (std::invalid_argument &err) {
		cerr << "Error in buildWayGeometry: " << err.what() << endl;
	}

	return MultiLinestring(); // return a blank geometry
}

// Add a node geometry
void OutputObject::buildNodeGeometry(LatpLon ll, TileBbox *bboxPtr, vector_tile::Tile_Feature *featurePtr) const {
	featurePtr->add_geometry(9);					// moveTo, repeat x1
	pair<int,int> xy = bboxPtr->scaleLatpLon(ll.latp/10000000.0, ll.lon/10000000.0);
	featurePtr->add_geometry((xy.first  << 1) ^ (xy.first  >> 31));
	featurePtr->add_geometry((xy.second << 1) ^ (xy.second >> 31));
	featurePtr->set_type(vector_tile::Tile_GeomType_POINT);
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

bool operator==(const OutputObject &x, const OutputObject &y) {
	return
		x.layer == y.layer &&
		x.geomType == y.geomType &&
		x.attributes == y.attributes &&
		x.objectID == y.objectID;
}

// Do lexicographic comparison, with the order of: layer, geomType, attributes, and objectID.
// Note that attributes is preffered to objectID.
// It is to arrange objects with the identical attributes continuously.
// Such objects will be merged into one object, to reduce the size of output.
bool operator<(const OutputObject &x, const OutputObject &y) {
	if (x.layer < y.layer) return true;
	if (x.layer > y.layer) return false;
	if (x.geomType < y.geomType) return true;
	if (x.geomType > y.geomType) return false;
	if (x.attributes < y.attributes) return true;
	if (x.attributes > y.attributes) return false;
	if (x.objectID < y.objectID) return true;
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

