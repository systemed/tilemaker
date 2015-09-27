struct WayStore {
	vector <uint32_t> nodelist;
};

/*
	OutputObject - any object (node, linestring, polygon) to be outputted to tiles

	Possible future improvements:
	- pack WayStore.nodelist (e.g. zigzag PBF encoding)
	- use a global dictionary for attribute key/values (another level of indirection, but would save memory)
*/

typedef vector<pair<int,int>> XYString;

class OutputObject { public:

	vector_tile::Tile_GeomType geomType;				// point, linestring, polygon? (UNKNOWN = centroid from polygon)
	uint_least8_t layer;								// what layer is it in?
	uint32_t osmID;										// id of way (linestring/polygon) or node (point)
	map <string, vector_tile::Tile_Value> attributes;	// attributes
	vector<uint32_t> innerWays;							// multipolygons - ways contained in this
	vector<uint32_t> outerWays;							//  |

	OutputObject(vector_tile::Tile_GeomType type, uint_least8_t l, uint32_t id) {
		geomType = type;
		layer = l;
		osmID = id;
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
	void buildWayGeometry(const node_container_t &nodes, map< uint32_t, WayStore > *waysPtr, TileBbox *bboxPtr, vector_tile::Tile_Feature *featurePtr) const {
		uint32_t objID = osmID;
		if (outerWays.size()>0) { objID = outerWays[0]; }
		const vector <uint32_t> &nodelist = waysPtr->at(objID).nodelist;
		vector<Point> points;
		
		if (geomType==vector_tile::Tile_GeomType_POLYGON || geomType==vector_tile::Tile_GeomType_UNKNOWN) {
			// polygon
			MultiPolygon mp;
			MultiPolygon out;

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
			if (geomType==vector_tile::Tile_GeomType_UNKNOWN) {
				// centroid only
				Point p;
				geom::centroid(mp, p);
				if (geom::within(p, bboxPtr->clippingBox)) {
					featurePtr->add_geometry(9);					// moveTo, repeat x1
					pair<int,int> xy = bboxPtr->scaleLatLon(p.y(), p.x());
					featurePtr->add_geometry((xy.first  << 1) ^ (xy.first  >> 31));
					featurePtr->add_geometry((xy.second << 1) ^ (xy.second >> 31));
				}
				
			} else {
				// full polygon
				geom::intersection(bboxPtr->clippingBox, mp, out); // clip

				pair<int,int> lastPos(0,0);
				for (MultiPolygon::const_iterator it = out.begin(); it != out.end(); ++it) {
					XYString scaledString;
					Ring ring = geom::exterior_ring(*it);
					for (auto jt = ring.begin(); jt != ring.end(); ++jt) {
						pair<int,int> xy = bboxPtr->scaleLatLon(jt->get<1>(), jt->get<0>());
						scaledString.push_back(xy);
					}
					writeDeltaString(&scaledString, featurePtr, &lastPos);

					InteriorRing interiors = geom::interior_rings(*it);
					for (auto ii = interiors.begin(); ii != interiors.end(); ++ii) {
						scaledString.clear();
						XYString scaledInterior;
						for (auto jt = ii->begin(); jt != ii->end(); ++jt) {
							pair<int,int> xy = bboxPtr->scaleLatLon(jt->get<1>(), jt->get<0>());
							scaledString.push_back(xy);
						}
						writeDeltaString(&scaledString, featurePtr, &lastPos);
					}
				}
			}

		} else { 
			// linestring
			Linestring ls;
			fillPointArray(points, nodelist, nodes);
			geom::assign_points(ls, points);
			// clip
			MultiLinestring out;
			geom::intersection(ls, bboxPtr->clippingBox, out);
			// write out
			pair<int,int> lastPos(0,0);
			for (MultiLinestring::const_iterator it = out.begin(); it != out.end(); ++it) {
				XYString scaledString;
				for (Linestring::const_iterator jt = it->begin(); jt != it->end(); ++jt) {
					pair<int,int> xy = bboxPtr->scaleLatLon(jt->get<1>(), jt->get<0>());
					scaledString.push_back(xy);
				}
				writeDeltaString(&scaledString, featurePtr, &lastPos);
			}
		}
		
		// Set feature type
		featurePtr->set_type(geomType);
	}
	
	// Helper to make a vector of Boost points from a vector of node IDs
	void fillPointArray(vector<Point> &points, const vector<uint32_t> &nodelist, const node_container_t &nodes) const {
		points.clear();
		if (points.capacity() < nodelist.size()) { points.reserve(nodelist.size()); }
		for (auto node_id : nodelist) {
			LatLon ll = nodes.at(node_id);
			points.emplace_back(geom::make<Point>(ll.lon/10000000.0, ll.lat/10000000.0));
		}
	}
	
	// Add a node geometry 
	void buildNodeGeometry(LatLon ll, TileBbox *bboxPtr, vector_tile::Tile_Feature *featurePtr) const {
		featurePtr->add_geometry(9);					// moveTo, repeat x1
		pair<int,int> xy = bboxPtr->scaleLatLon(ll.lat/10000000.0, ll.lon/10000000.0);
		featurePtr->add_geometry((xy.first  << 1) ^ (xy.first  >> 31));
		featurePtr->add_geometry((xy.second << 1) ^ (xy.second >> 31));
		featurePtr->set_type(geomType);
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
	
	// Encode a series of pixel co-ordinates into the feature, using delta and zigzag encoding
	void writeDeltaString(XYString *scaledString, vector_tile::Tile_Feature *featurePtr, pair<int,int> *lastPos) const {
		if (scaledString->size()<2) return;
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
		for (uint i=1; i<scaledString->size(); i++) {
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
		if (len==0) return;
		geometry[3] = (len << 3) + 2;				// lineTo plus repeat
		if (geomType==vector_tile::Tile_GeomType_POLYGON) {
			geometry.push_back(7+8);				// closePath
		}
		for (uint i=0; i<geometry.size(); i++) { 
			featurePtr->add_geometry(geometry[i]);
		};
		lastPos->first  = lastX;
		lastPos->second = lastY;
	}

};

// Hashing function so we can use an unordered_set

bool operator==(const OutputObject& x, const OutputObject& y) {
	return (x.layer == y.layer) && (x.osmID == y.osmID);
}

namespace std {
	template<>
	struct hash<OutputObject> {
		size_t operator()(const OutputObject &oo) const {
			return std::hash<uint_least8_t>()(oo.layer) ^ std::hash<uint32_t>()(oo.osmID);
		}
	};
}
