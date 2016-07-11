/*
	OutputObject - any object (node, linestring, polygon) to be outputted to tiles

	Possible future improvements to save memory:
	- use a global dictionary for attribute key/values
*/

enum OutputGeometryType { POINT, LINESTRING, POLYGON, CENTROID, CACHED_POINT, CACHED_LINESTRING, CACHED_POLYGON };

class ClipGeometryVisitor : public boost::static_visitor<Geometry> {

	const Box &clippingBox;

public:
	ClipGeometryVisitor(const Box &cbox) : clippingBox(cbox) {};

	Geometry operator()(const Point &p) const {
		if (geom::within(p, clippingBox)) {
			return p;
		} else {
			return MultiLinestring(); // return a blank geometry
		}
	}

	Geometry operator()(const Linestring &ls) const {
		MultiLinestring out;
		geom::intersection(ls, clippingBox, out);
		return out;
	}

	Geometry operator()(const MultiLinestring &mls) const {
#if BOOST_VERSION <= 105800
		// Due to https://svn.boost.org/trac/boost/ticket/11268, we can't clip a MultiLinestring with Boost 1.56-1.58
		return mls;
#else
		MultiLinestring out;
		geom::intersection(mls, clippingBox, out);
		return out;
#endif
	}

	Geometry operator()(const MultiPolygon &mp) const {
		MultiPolygon out;
		geom::intersection(mp, clippingBox, out);
		return out;
	}
};

class OutputObject { public:

	OutputGeometryType geomType;						// point, linestring, polygon...
	uint_least8_t layer;								// what layer is it in?
	NodeID objectID;									// id of way (linestring/polygon) or node (point)
	map <string, vector_tile::Tile_Value> attributes;	// attributes

	OutputObject(OutputGeometryType type, uint_least8_t l, NodeID id) {
		geomType = type;
		layer = l;
		objectID = id;
	}
	
	void addAttribute(const string &key, vector_tile::Tile_Value &value) {
		attributes[key]=value;
	}

	// Assemble a linestring or polygon into a Boost geometry, and clip to bounding box
	// Returns a boost::variant -
	//   POLYGON->MultiPolygon, CENTROID->Point, LINESTRING->MultiLinestring
	Geometry buildWayGeometry(const OSMStore &osmStore,
	                      TileBbox *bboxPtr, 
	                      vector<Geometry> &cachedGeometries) const {

		ClipGeometryVisitor clip(bboxPtr->clippingBox);

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

		return MultiLinestring(); // return a blank geometry
	}
	
	// Add a node geometry
	void buildNodeGeometry(LatpLon ll, TileBbox *bboxPtr, vector_tile::Tile_Feature *featurePtr) const {
		featurePtr->add_geometry(9);					// moveTo, repeat x1
		pair<int,int> xy = bboxPtr->scaleLatpLon(ll.latp/10000000.0, ll.lon/10000000.0);
		featurePtr->add_geometry((xy.first  << 1) ^ (xy.first  >> 31));
		featurePtr->add_geometry((xy.second << 1) ^ (xy.second >> 31));
		featurePtr->set_type(vector_tile::Tile_GeomType_POINT);
	}
	
	// Write attribute key/value pairs (dictionary-encoded)
	void writeAttributes(vector<string> *keyList, vector<vector_tile::Tile_Value> *valueList, vector_tile::Tile_Feature *featurePtr) const {
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
	int findValue(vector<vector_tile::Tile_Value> *valueList, vector_tile::Tile_Value *value) const {
		for (int i=0; i<valueList->size(); i++) {
			vector_tile::Tile_Value v = valueList->at(i);
			if (v.has_string_value() && value->has_string_value() && v.string_value()==value->string_value()) { return i; }
			if (v.has_float_value()  && value->has_float_value()  && v.float_value() ==value->float_value() ) { return i; }
			if (v.has_bool_value()   && value->has_bool_value()   && v.bool_value()  ==value->bool_value()  ) { return i; }
		}
		return -1;
	}
};

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

// Hashing function so we can use an unordered_set

namespace std {
	template<>
	struct hash<OutputObject> {
		size_t operator()(const OutputObject &oo) const {
			return std::hash<uint_least8_t>()(oo.layer) ^ std::hash<NodeID>()(oo.objectID);
		}
	};
}
