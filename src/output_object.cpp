struct WayStore {
	vector <uint32_t> nodelist;
};

/*
	OutputObject - any object (node, linestring, polygon) to be outputted to tiles

	Possible future improvements to save memory:
	- pack WayStore.nodelist (e.g. zigzag PBF encoding)
	- use a global dictionary for attribute key/values
	- combine innerWays and outerWays into one vector, with a single-byte index marking the changeover
*/

enum OutputGeometryType { POINT, LINESTRING, POLYGON, CENTROID, CACHED_POINT, CACHED_LINESTRING, CACHED_POLYGON };

class OutputObject { public:

	OutputGeometryType geomType;						// point, linestring, polygon...
	uint_least8_t layer;								// what layer is it in?
	uint32_t objectID;									// id of way (linestring/polygon) or node (point)
	map <string, vector_tile::Tile_Value> attributes;	// attributes
	vector<uint32_t> innerWays;							// multipolygons - ways contained in this
	vector<uint32_t> outerWays;							//  |

	OutputObject(OutputGeometryType type, uint_least8_t l, uint32_t id) {
		geomType = type;
		layer = l;
		objectID = id;
	}
	
	void addAttribute(const string &key, vector_tile::Tile_Value &value) {
		attributes[key]=value;
	}

	void addRelationWay(uint32_t wayID, bool isInner) {
		if (isInner) { innerWays.push_back(wayID); }
		        else { outerWays.push_back(wayID); }
	}

	// Assemble a linestring or polygon into a Boost geometry, and clip to bounding box
	// (the linestring code is the easiest way to understand this - the polygon code 
	//  is greatly complicated by multipolygon support)
	// Returns a boost::variant -
	//   POLYGON->MultiPolygon, CENTROID->Point, LINESTRING->MultiLinestring
	Geometry buildWayGeometry(const node_container_t &nodes,
	                      map< uint32_t, WayStore > *waysPtr, 
	                      TileBbox *bboxPtr, 
	                      vector<Geometry> &cachedGeometries) const {
		uint32_t objID = objectID;
		if (outerWays.size()>0) { objID = outerWays[0]; }
		vector<Point> points;
		
		if (geomType==POLYGON || geomType==CENTROID) {
			// polygon
			const vector <uint32_t> &nodelist = waysPtr->at(objID).nodelist;
			MultiPolygon mp;

			// main outer way and inners
			Polygon poly;
			fillPointArray(points, nodelist, nodes);
			geom::assign_points(poly, points);
			geom::interior_rings(poly).resize(innerWays.size());
			for (uint i=0; i<innerWays.size(); i++) {
				fillPointArray(points, waysPtr->at(innerWays[i]).nodelist, nodes);
				geom::append(poly, points, i);
			}
			mp.push_back(poly);

			// additional outer ways - we don't match them up with inners, that shit is insane
			for (uint i = 1; i < outerWays.size(); i++) {
				Polygon outer;
				fillPointArray(points, waysPtr->at(outerWays[i]).nodelist, nodes);
				geom::assign_points(outer, points);
				mp.push_back(outer);
			}

			// fix winding
			geom::correct(mp);

			// write out
			if (geomType==CENTROID) {
				// centroid only
				Point p;
				geom::centroid(mp, p);
				if (geom::within(p, bboxPtr->clippingBox)) { return p; }

			} else {
				// full polygon
				MultiPolygon out;
				geom::intersection(bboxPtr->clippingBox, mp, out); // clip
				return out;
			}

		} else if (geomType==LINESTRING) {
			// linestring
			Linestring ls;
			const vector <uint32_t> &nodelist = waysPtr->at(objID).nodelist;
			fillPointArray(points, nodelist, nodes);
			geom::assign_points(ls, points);
			// clip
			MultiLinestring out;
			geom::intersection(ls, bboxPtr->clippingBox, out);
			return out;

		} else if (geomType==CACHED_LINESTRING || geomType==CACHED_POLYGON || geomType==CACHED_POINT) {
			return cachedGeometries[objectID];
		}

		MultiLinestring out; return out; // return a blank geometry
	}

	// Helper to make a vector of Boost points from a vector of node IDs
	void fillPointArray(vector<Point> &points, const vector<uint32_t> &nodelist, const node_container_t &nodes) const {
		points.clear();
		if (points.capacity() < nodelist.size()) { points.reserve(nodelist.size()); }
		for (auto node_id : nodelist) {
			LatpLon ll = nodes.at(node_id);
			points.emplace_back(geom::make<Point>(ll.lon/10000000.0, ll.latp/10000000.0));
		}
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

// Hashing function so we can use an unordered_set

bool operator==(const OutputObject& x, const OutputObject& y) {
	return (x.layer == y.layer) && (x.objectID == y.objectID) && (x.geomType == y.geomType);
}

namespace std {
	template<>
	struct hash<OutputObject> {
		size_t operator()(const OutputObject &oo) const {
			return std::hash<uint_least8_t>()(oo.layer) ^ std::hash<uint32_t>()(oo.objectID);
		}
	};
}
