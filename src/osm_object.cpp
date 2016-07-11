struct LayerDef {
	string name;
	int minzoom;
	int maxzoom;
	int simplifyBelow;
	double simplifyLevel;
	double simplifyLength;
	double simplifyRatio;
};

/*
	OSMObject - represents the object (from the .osm.pbf) currently being processed
	
	Only one instance of this class is ever used. Its main purpose is to provide a 
	consistent object for Luabind to access.
	
*/

class OSMObject { public:

	lua_State *luaState;					// Lua reference
	map<string, RTree> *indices;			// Spatial indices
	vector<Geometry> *cachedGeometries;		// Cached geometries
	map<uint,string> *cachedGeometryNames;	// Cached geometry names
	OSMStore *osmStore;						// Global OSM store

	uint64_t osmID;							// ID of OSM object
	uint32_t newWayID = MAX_WAY_ID;			// Decrementing new ID for relations
	bool isWay, isRelation;					// Way, node, relation?

	int32_t lon1,latp1,lon2,latp2;			// Start/end co-ordinates of OSM object
	NodeVec *nodeVec;						// node vector
	WayVec *outerWayVec, *innerWayVec;		// way vectors

	Linestring linestringCache;
	bool linestringInited;
	Polygon polygonCache;
	bool polygonInited;
	MultiPolygon multiPolygonCache;
	bool multiPolygonInited;

	vector<LayerDef> layers;				// List of layers
	map<string,uint> layerMap;				// Layer->position map
	vector<vector<uint>> layerOrder;		// Order of (grouped) layers, e.g. [ [0], [1,2,3], [4] ]

	vector<OutputObject> outputs;			// All output objects

	// Common tag storage
	vector<string> stringTable;				// Tag table from the current PrimitiveGroup
	map<string, int> tagMap;				// String->position map

	// Tag storage for denseNodes
	int denseStart;							// Start of key/value table section (DenseNodes)
	int denseEnd;							// End of key/value table section (DenseNodes)
	DenseNodes *densePtr;					// DenseNodes object

	// Tag storage for ways/relations
	::google::protobuf::RepeatedField< ::google::protobuf::uint32 > *keysPtr;
	::google::protobuf::RepeatedField< ::google::protobuf::uint32 > *valsPtr;
	int tagLength;

	// ----	initialization routines

	OSMObject(lua_State *luaPtr, map< string, RTree> *idxPtr, vector<Geometry> *geomPtr, map<uint,string> *namePtr, OSMStore *storePtr) {
		luaState = luaPtr;
		indices = idxPtr;
		cachedGeometries = geomPtr;
		cachedGeometryNames = namePtr;
		osmStore = storePtr;
	}

	// Define a layer (as read from the .json file)
	uint addLayer(string name, int minzoom, int maxzoom,
			int simplifyBelow, double simplifyLevel, double simplifyLength, double simplifyRatio, string writeTo) {
		LayerDef layer = { name, minzoom, maxzoom, simplifyBelow, simplifyLevel, simplifyLength, simplifyRatio };
		layers.push_back(layer);
		uint layerNum = layers.size()-1;
		layerMap[name] = layerNum;

		if (writeTo.empty()) {
			vector<uint> r = { layerNum };
			layerOrder.push_back(r);
		} else {
			if (layerMap.count(writeTo) == 0) {
				throw out_of_range("ERROR: addLayer(): the layer to write, named as \"" + writeTo + "\", doesn't exist.");
			}
			uint lookingFor = layerMap[writeTo];
			for (auto it = layerOrder.begin(); it!= layerOrder.end(); ++it) {
				if (it->at(0)==lookingFor) {
					it->push_back(layerNum);
				}
			}
		}
		return layerNum;
	}

	// Read string dictionary from the .pbf
	void readStringTable(PrimitiveBlock *pbPtr) {
		uint i;
		// Populate the string table
		stringTable.clear();
		stringTable.resize(pbPtr->stringtable().s_size());
		for (i=0; i<pbPtr->stringtable().s_size(); i++) {
			stringTable[i] = pbPtr->stringtable().s(i);
		}
		// Create a string->position map
		tagMap.clear();
		for (i=0; i<pbPtr->stringtable().s_size(); i++) {
			tagMap.insert(pair<string, int> (pbPtr->stringtable().s(i), i));
		}
	}

	// ----	Helpers provided for main routine

	// Has this object been assigned to any layers?
	bool empty() {
		return outputs.size()==0;
	}

	// Find a string in the dictionary
	int findStringPosition(string str) {
		auto p = find(stringTable.begin(), stringTable.end(), str);
		if (p == stringTable.end()) {
			return -1;
		} else {
			return distance(stringTable.begin(), p);
		}
	}

	// ----	Set an osm element to make it accessible from Lua

	// We are now processing a node
	inline void setNode(NodeID id, DenseNodes *dPtr, int kvStart, int kvEnd, LatpLon node) {
		reset();
		osmID = id;
		isWay = false;
		isRelation = false;

		setLocation(node.lon, node.latp, node.lon, node.latp);

		denseStart = kvStart;
		denseEnd = kvEnd;
		densePtr = dPtr;
	}

	// We are now processing a way
	inline void setWay(Way *way, NodeVec *nodeVecPtr) {
		reset();
		osmID = way->id();
		isWay = true;
		isRelation = false;

		nodeVec = nodeVecPtr;
		setLocation(osmStore->nodes.at(nodeVec->front()).lon, osmStore->nodes.at(nodeVec->front()).latp,
				osmStore->nodes.at(nodeVec->back()).lon, osmStore->nodes.at(nodeVec->back()).latp);

		keysPtr = way->mutable_keys();
		valsPtr = way->mutable_vals();
		tagLength = way->keys_size();
	}

	// We are now processing a relation
	// (note that we store relations as ways with artificial IDs, and that
	//  we use decrementing positive IDs to give a bit more space for way IDs)
	inline void setRelation(Relation *relation, WayVec *outerWayVecPtr, WayVec *innerWayVecPtr) {
		reset();
		osmID = --newWayID;
		isWay = true;
		isRelation = true;

		outerWayVec = outerWayVecPtr;
		innerWayVec = innerWayVecPtr;
		//setLocation(...); TODO

		keysPtr = relation->mutable_keys();
		valsPtr = relation->mutable_vals();
		tagLength = relation->keys_size();
	}

	// Internal: clear current cached state
	inline void reset() {
		outputs.clear();
		linestringInited = false;
		polygonInited = false;
		multiPolygonInited = false;
	}

	// Internal: set start/end co-ordinates
	inline void setLocation(int32_t a, int32_t b, int32_t c, int32_t d) {
		lon1=a; latp1=b; lon2=c; latp2=d;
	}

	// ----	Metadata queries called from Lua

	// Get the ID of the current object
	string Id() const {
		return to_string(osmID);
	}

	// Check if there's a value for a given key
	bool Holds(const string& key) const {
		if (tagMap.find(key) == tagMap.end()) { return false; }
		uint keyNum = tagMap.at(key);
		if (isWay) {
			for (uint n=0; n > tagLength; n++) {
				if (keysPtr->Get(n)==keyNum) { return true; }
			}
		} else {
			for (uint n=denseStart; n<denseEnd; n+=2) {
				if (densePtr->keys_vals(n)==keyNum) { return true; }
			}
		}
		return false;
	}

	// Get an OSM tag for a given key (or return empty string if none)
	string Find(const string& key) const {
		// First, convert the string into a number
		if (tagMap.find(key) == tagMap.end()) { return ""; }
		uint keyNum = tagMap.at(key);
		if (isWay) {
			// Then see if this number is in the way tags, and return its value if so
			for (uint n=0; n < tagLength; n++) {
				if (keysPtr->Get(n)==keyNum) { return stringTable[valsPtr->Get(n)]; }
			}
		} else {
			for (uint n=denseStart; n<denseEnd; n+=2) {
				if (densePtr->keys_vals(n)==keyNum) { return stringTable[densePtr->keys_vals(n+1)]; }
			}
		}
		return "";
	}

	// ----	Spatial queries called from Lua

	// Find intersecting shapefile layer
	// TODO: multipolygon relations not supported, will always return false
	vector<string> FindIntersecting(const string &layerName) {
		vector<uint> ids = findIntersectingGeometries(layerName);
		return namesOfGeometries(ids);
	}
	bool Intersects(const string &layerName) {
		return !findIntersectingGeometries(layerName).empty();
	}
	vector<uint> findIntersectingGeometries(const string &layerName) {
		vector<IndexValue> results;
		vector<uint> ids;

		auto f = indices->find(layerName);
		if (f==indices->end()) {
			cerr << "Couldn't find indexed layer " << layerName << endl;
		} else if (!isWay) {
			Point p(lon1/10000000.0,latp1/10000000.0);
			f->second.query(geom::index::intersects(p), back_inserter(results));
			return verifyIntersectResults(results,p,p);
		} else if (!isRelation) {
			Point p1(lon1/10000000.0,latp1/10000000.0);
			Point p2(lon1/10000000.0,latp1/10000000.0);
			Box box = Box(p1,p2);
			f->second.query(geom::index::intersects(box), back_inserter(results));
			return verifyIntersectResults(results,p1,p2);
		}
		return vector<uint>();	// empty, relations not supported
	}
	vector<uint> verifyIntersectResults(vector<IndexValue> &results, Point &p1, Point &p2) {
		vector<uint> ids;
		for (auto it : results) {
			uint id=it.second;
			if      (         geom::intersects(cachedGeometries->at(id),p1)) { ids.push_back(id); }
			else if (isWay && geom::intersects(cachedGeometries->at(id),p2)) { ids.push_back(id); }
		}
		return ids;
	}
	vector<string> namesOfGeometries(vector<uint> &ids) {
		vector<string> names;
		for (uint i=0; i<ids.size(); i++) {
			if (cachedGeometryNames->find(ids[i])!=cachedGeometryNames->end()) {
				names.push_back(cachedGeometryNames->at(ids[i]));
			}
		}
		return names;
	}

	// Returns whether it is closed polygon
	bool IsClosed() const {
		if (!isWay) return false; // nonsense: it isn't a way
		if (isRelation) {
			return true; // TODO: check it when non-multipolygon are supported
		} else {
			return nodeVec->front() == nodeVec->back();
		}
	}

	// Scale to (kilo)meter
	double ScaleToMeter() {
		return degp2meter(1.0, (latp1/2+latp2/2)/10000000.0);
	}

	double ScaleToKiloMeter() {
		return (1/1000.0) * ScaleToMeter();
	}

	// Returns area
	double Area() {
		if (!IsClosed()) return 0;
		if (isRelation) {
			return geom::area(multiPolygon());
		} else if (isWay) {
			return geom::area(polygon());
		} else {
			return 0;
		}
	}

	// Returns length
	double Length() {
		if (isRelation) {
			return geom::length(multiPolygon());
		} else if (isWay) {
			return geom::length(linestring());
		} else {
			return 0;
		}
	}

	// Lazy geometries creation
	const Linestring &linestring() {
		if (!linestringInited) {
			linestringInited = true;
			linestringCache = osmStore->nodeListLinestring(*nodeVec);
		}
		return linestringCache;
	}

	const Polygon &polygon() {
		if (!polygonInited) {
			polygonInited = true;
			polygonCache = osmStore->nodeListPolygon(*nodeVec);
		}
		return polygonCache;
	}

	const MultiPolygon &multiPolygon() {
		if (!multiPolygonInited) {
			multiPolygonInited = true;
			multiPolygonCache = osmStore->wayListMultiPolygon(*outerWayVec, *innerWayVec);
		}
		return multiPolygonCache;
	}

	// ----	Requests from Lua to write this way/node to a vector tile's Layer

	// Add layer
	void Layer(const string &layerName, bool area) {
		if (layerMap.count(layerName) == 0) {
			throw out_of_range("ERROR: Layer(): a layer named as \"" + layerName + "\" doesn't exist.");
		}
		OutputObject oo(isWay ? (area ? POLYGON : LINESTRING) : POINT,
						layerMap[layerName],
						osmID);
		outputs.push_back(oo);
	}
	void LayerAsCentroid(const string &layerName) {
		if (layerMap.count(layerName) == 0) {
			throw out_of_range("ERROR: LayerAsCentroid(): a layer named as \"" + layerName + "\" doesn't exist.");
		}
		OutputObject oo(CENTROID,
						layerMap[layerName],
						osmID);
		outputs.push_back(oo);
	}
	
	// Set attributes in a vector tile's Attributes table
	void Attribute(const string &key, const string &val) {
		if (val.size()==0) { return; }		// don't set empty strings
		if (outputs.size()==0) { cerr << "Can't add Attribute " << key << " if no Layer set" << endl; return; }
		vector_tile::Tile_Value v;
		v.set_string_value(val);
		outputs[outputs.size()-1].addAttribute(key, v);
	}
	void AttributeNumeric(const string &key, const float val) {
		if (outputs.size()==0) { cerr << "Can't add Attribute " << key << " if no Layer set" << endl; return; }
		vector_tile::Tile_Value v;
		v.set_float_value(val);
		outputs[outputs.size()-1].addAttribute(key, v);
	}
	void AttributeBoolean(const string &key, const bool val) {
		if (outputs.size()==0) { cerr << "Can't add Attribute " << key << " if no Layer set" << endl; return; }
		vector_tile::Tile_Value v;
		v.set_bool_value(val);
		outputs[outputs.size()-1].addAttribute(key, v);
	}
};
