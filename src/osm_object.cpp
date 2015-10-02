struct LayerDef {
	string name;
	int minzoom;
	int maxzoom;
	int simplifyBelow;
	double simplifyLevel;
};

/*
	OSMObject - represents the object (from the .osm.pbf) currently being processed
	
	Only one instance of this class is ever used. Its main purpose is to provide a 
	consistent object for Luabind to access.
	
*/

class OSMObject { public:

	lua_State *luaState;					// Lua reference
	map< string, RTree> *indices;			// Spatial indices
	vector<Geometry> *cachedGeometries;		// Cached geometries
	map<uint,string> *cachedGeometryNames;	// Cached geometry names
	node_container_t *nodes;				// Node storage
	bool isWay, isRelation;					// Way, node, relation?
	uint32_t osmID;							// ID of OSM object
	uint32_t newWayID = 4294967295;			// Decrementing new ID for relations
	int32_t lon1,latp1,lon2,latp2;			// Start/end co-ordinates of OSM object

	vector<LayerDef> layers;				// List of layers
	map<string,uint> layerMap;				// Layer->position map
	vector< vector<uint> > layerOrder;		// Order of (grouped) layers, e.g. [ [0], [1,2,3], [4] ]

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

	OSMObject(lua_State *luaPtr, map< string, RTree> *idxPtr, vector<Geometry> *geomPtr, map<uint,string> *namePtr, node_container_t *nodePtr) {
		luaState = luaPtr;
		indices = idxPtr;
		cachedGeometries = geomPtr;
		cachedGeometryNames = namePtr;
		nodes = nodePtr;
	}

	// Define a layer (as read from the .json file)
	uint addLayer(string name, int minzoom, int maxzoom, int simplifyBelow, double simplifyLevel, string writeTo) {
		LayerDef layer = { name, minzoom, maxzoom, simplifyBelow, simplifyLevel };
		layers.push_back(layer);
		uint layerNum = layers.size()-1;
		layerMap[name] = layerNum;

		if (writeTo.empty()) {
			vector<uint> r = { layerNum };
			layerOrder.push_back(r);
		} else {
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
	
	// We are now processing a way
	inline void setWay(Way *way) {
		outputs.clear();
		keysPtr = way->mutable_keys();
		valsPtr = way->mutable_vals();
		tagLength = way->keys_size();
		osmID = way->id();
		isWay = true;
		isRelation = false;
	}
	
	// We are now processing a node
	inline void setNode(uint32_t id, DenseNodes *dPtr, int kvStart, int kvEnd) {
		outputs.clear();
		osmID = id;
		isWay = false;
		isRelation = false;
		denseStart = kvStart;
		denseEnd = kvEnd;
		densePtr = dPtr;
	}
	
	// We are now processing a relation
	// (note that we store relations as ways with artificial IDs, and that
	//  we use decrementing positive IDs to give a bit more space for way IDs)
	inline void setRelation(Relation *relation) {
		outputs.clear();
		keysPtr = relation->mutable_keys();
		valsPtr = relation->mutable_vals();
		tagLength = relation->keys_size();
		osmID = --newWayID;
		isWay = true;
		isRelation = true;
	}

	// Set start/end co-ordinates
	inline void setLocation(int32_t a, int32_t b, int32_t c, int32_t d) {
		lon1=a; latp1=b; lon2=c; latp2=d;
	}
	
	// Read relation members and store them in each OutputObject
	// (required for multipolygons, but will be useful for other type of relations)
	// Also make a note (in the way->relations map) to read each way, even if it's 
	// not rendered in its own right
	void storeRelationWays(Relation *relation, map<uint32_t, vector<uint32_t>> *wayRelations, int innerKey, int outerKey) {
		int64_t lastID = 0;
		for (uint n=0; n < relation->memids_size(); n++) {
			lastID += relation->memids(n);
			if (relation->types(n) != Relation_MemberType_WAY) { continue; }
			int32_t role = relation->roles_sid(n);
			// if (role != innerKey && role != outerKey) { continue; }
			// ^^^^ commented out so that we don't die horribly when a relation has no outer way
			uint32_t wayID = static_cast<uint32_t>(lastID);
			// Store this relation in the way->relations map
			if (wayRelations->count(wayID)==0) { wayRelations->insert(make_pair(wayID,vector<uint32_t>())); }
			wayRelations->at(wayID).push_back(osmID);
			// Add the way ID into each of the relation's OutputObjects
			for (auto jt = outputs.begin(); jt != outputs.end(); ++jt) {
				jt->addRelationWay(wayID, role==innerKey);
			}
		}
	}
	
	// Write this way/node to a vector tile's Layer
	// Called from Lua
	void Layer(const string &layerName, bool area) {
		OutputObject oo(isWay ? (area ? POLYGON : LINESTRING) : POINT,
						layerMap[layerName],
						osmID);
		outputs.push_back(oo);
	}
	void LayerAsCentroid(const string &layerName) {
		OutputObject oo(CENTROID,
						layerMap[layerName],
						osmID);
		outputs.push_back(oo);
	}
	
	// Set attributes in a vector tile's Attributes table
	// Called from Lua
	void Attribute(const string &key, const string &val) {
		if (val.size()==0) { return; }		// don't set empty strings
		vector_tile::Tile_Value v;
		v.set_string_value(val);
		outputs[outputs.size()-1].addAttribute(key, v);
	}
	void AttributeNumeric(const string &key, const float &val) {
		vector_tile::Tile_Value v;
		v.set_float_value(val);
		outputs[outputs.size()-1].addAttribute(key, v);
	}
	void AttributeBoolean(const string &key, const bool val) {
		vector_tile::Tile_Value v;
		v.set_bool_value(val);
		outputs[outputs.size()-1].addAttribute(key, v);
	}
	
	// Query spatial indexes
	// Called from Lua
	// Note - multipolygon relations not supported, will always return false (because we don't know the geometry yet)
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
		if (!isWay) {
			Point p(lon1/10000000.0,latp1/10000000.0);
			indices->at(layerName).query(geom::index::intersects(p), back_inserter(results));
			return verifyIntersectResults(results,p,p);
		} else if (!isRelation) {
			Point p1(lon1/10000000.0,latp1/10000000.0);
			Point p2(lon1/10000000.0,latp1/10000000.0);
			Box box = Box(p1,p2);
			indices->at(layerName).query(geom::index::intersects(box), back_inserter(results));
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

	// Get an OSM tag for a given key (or return empty string if none)
	// Called from Lua
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

	// Check if there's a value for a given key
	// Called from Lua
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
	
	// Get the ID of the current object
	// Called from Lua	
	string Id() const {
		return to_string(osmID);
	}
	
};
