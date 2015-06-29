struct LayerDef {
	string name;
	int minzoom;
	int maxzoom;
};

/*
	OSMObject - represents the object (from the .osm.pbf) currently being processed
	
	Only one instance of this class is ever used. Its main purpose is to provide a 
	consistent object for Luabind to access.
	
*/

class OSMObject { public:

	lua_State *luaState;					// Lua reference
	bool isWay;								// Way or node?
	uint32_t osmID;							// ID of OSM object
	uint32_t newID = 4294967295;			// decrementing new ID for relations

	vector<LayerDef> layers;				// List of layers
	map<string,uint> layerMap;				// Layer->position map

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

	OSMObject(lua_State *luaPtr) {
		luaState = luaPtr;
	}

	// Define a layer (as read from the .json file)
	void addLayer(string name, int minzoom, int maxzoom) {
		LayerDef layer = { name, minzoom, maxzoom };
		layers.push_back(layer);
		layerMap[name] = layers.size()-1;
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
	void setWay(Way *way) {
		outputs.clear();
		keysPtr = way->mutable_keys();
		valsPtr = way->mutable_vals();
		tagLength = way->keys_size();
		osmID = way->id();
		isWay = true;
	}
	
	// We are now processing a node
	void setNode(uint32_t id, DenseNodes *dPtr, int kvStart, int kvEnd) {
		outputs.clear();
		osmID = id;
		isWay = false;
		denseStart = kvStart;
		denseEnd = kvEnd;
		densePtr = dPtr;
	}
	
	// We are now processing a relation
	// (note that we store relations as ways with artificial IDs, and that
	//  we use decrementing positive IDs to give a bit more space for way IDs)
	void setRelation(Relation *relation) {
		outputs.clear();
		keysPtr = relation->mutable_keys();
		valsPtr = relation->mutable_vals();
		tagLength = relation->keys_size();
		osmID = --newID;
		isWay = true;
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
		OutputObject oo(isWay ? (area ? vector_tile::Tile_GeomType_POLYGON : vector_tile::Tile_GeomType_LINESTRING) : vector_tile::Tile_GeomType_POINT,
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
