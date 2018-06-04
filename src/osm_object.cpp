#include "osm_object.h"
using namespace std;
using namespace rapidjson;

// ----	initialization routines

OSMObject::OSMObject(class Config &configIn, kaguya::State &luaObj, 
	vector<Geometry> &geomPtr, map<uint,string> &namePtr, OSMStore *storePtr):
	luaState(luaObj),
	cachedGeometries(geomPtr),
	cachedGeometryNames(namePtr),
	config(configIn)
{
	newWayID = MAX_WAY_ID;
	osmStore = storePtr;
}

// Read string dictionary from the .pbf
void OSMObject::readStringTable(PrimitiveBlock *pbPtr) {
	// Populate the string table
	stringTable.clear();
	stringTable.resize(pbPtr->stringtable().s_size());
	for (int i=0; i<pbPtr->stringtable().s_size(); i++) {
		stringTable[i] = pbPtr->stringtable().s(i);
	}
	// Create a string->position map
	tagMap.clear();
	for (int i=0; i<pbPtr->stringtable().s_size(); i++) {
		tagMap.insert(pair<string, int> (pbPtr->stringtable().s(i), i));
	}
}

// ----	Helpers provided for main routine

// Has this object been assigned to any layers?
bool OSMObject::empty() {
	return outputs.size()==0;
}

// Find a string in the dictionary
int OSMObject::findStringPosition(string str) {
	auto p = find(stringTable.begin(), stringTable.end(), str);
	if (p == stringTable.end()) {
		return -1;
	} else {
		return distance(stringTable.begin(), p);
	}
}

// ----	Metadata queries called from Lua

// Get the ID of the current object
string OSMObject::Id() const {
	return to_string(osmID);
}

// Check if there's a value for a given key
bool OSMObject::Holds(const string& key) const {
	if (tagMap.find(key) == tagMap.end()) { return false; }
	uint keyNum = tagMap.at(key);
	if (isWay) {
		for (uint n=0; n > tagLength; n++) {
			if (keysPtr->Get(n)==keyNum) { return true; }
		}
	} else {
		for (uint n=denseStart; n<denseEnd; n+=2) {
			if (uint(densePtr->keys_vals(n))==keyNum) { return true; }
		}
	}
	return false;
}

// Get an OSM tag for a given key (or return empty string if none)
string OSMObject::Find(const string& key) const {
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
			if (uint(densePtr->keys_vals(n))==keyNum) { return stringTable[densePtr->keys_vals(n+1)]; }
		}
	}
	return "";
}

// ----	Spatial queries called from Lua

// Find intersecting shapefile layer
// TODO: multipolygon relations not supported, will always return false
vector<string> OSMObject::FindIntersecting(const string &layerName) {
	vector<uint> ids = findIntersectingGeometries(layerName);
	return namesOfGeometries(ids);
}
bool OSMObject::Intersects(const string &layerName) {
	return !findIntersectingGeometries(layerName).empty();
}
vector<uint> OSMObject::findIntersectingGeometries(const string &layerName) {
	vector<IndexValue> results;
	vector<uint> ids;

	auto f = indices.find(layerName);
	if (f==indices.end()) {
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
vector<uint> OSMObject::verifyIntersectResults(vector<IndexValue> &results, Point &p1, Point &p2) {
	vector<uint> ids;
	for (auto it : results) {
		uint id=it.second;
		if      (         geom::intersects(cachedGeometries.at(id),p1)) { ids.push_back(id); }
		else if (isWay && geom::intersects(cachedGeometries.at(id),p2)) { ids.push_back(id); }
	}
	return ids;
}
vector<string> OSMObject::namesOfGeometries(vector<uint> &ids) {
	vector<string> names;
	for (uint i=0; i<ids.size(); i++) {
		if (cachedGeometryNames.find(ids[i])!=cachedGeometryNames.end()) {
			names.push_back(cachedGeometryNames.at(ids[i]));
		}
	}
	return names;
}

// Returns whether it is closed polygon
bool OSMObject::IsClosed() const {
	if (!isWay) return false; // nonsense: it isn't a way
	if (isRelation) {
		return true; // TODO: check it when non-multipolygon are supported
	} else {
		return nodeVec->front() == nodeVec->back();
	}
}

// Scale to (kilo)meter
double OSMObject::ScaleToMeter() {
	return degp2meter(1.0, (latp1/2+latp2/2)/10000000.0);
}

double OSMObject::ScaleToKiloMeter() {
	return (1/1000.0) * ScaleToMeter();
}

// Returns area
double OSMObject::Area() {
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
double OSMObject::Length() {
	if (isRelation) {
		return geom::length(multiPolygon());
	} else if (isWay) {
		return geom::length(linestring());
	} else {
		return 0;
	}
}

// Lazy geometries creation
const Linestring &OSMObject::linestring() {
	if (!linestringInited) {
		linestringInited = true;
		linestringCache = osmStore->nodeListLinestring(*nodeVec);
	}
	return linestringCache;
}

const Polygon &OSMObject::polygon() {
	if (!polygonInited) {
		polygonInited = true;
		polygonCache = osmStore->nodeListPolygon(*nodeVec);
	}
	return polygonCache;
}

const MultiPolygon &OSMObject::multiPolygon() {
	if (!multiPolygonInited) {
		multiPolygonInited = true;
		multiPolygonCache = osmStore->wayListMultiPolygon(*outerWayVec, *innerWayVec);
	}
	return multiPolygonCache;
}

// ----	Requests from Lua to write this way/node to a vector tile's Layer

// Add layer
void OSMObject::Layer(const string &layerName, bool area) {
	if (config.layerMap.count(layerName) == 0) {
		throw out_of_range("ERROR: Layer(): a layer named as \"" + layerName + "\" doesn't exist.");
	}
	OutputObject oo(isWay ? (area ? POLYGON : LINESTRING) : POINT,
					config.layerMap[layerName],
					osmID);
	outputs.push_back(oo);
}
void OSMObject::LayerAsCentroid(const string &layerName) {
	if (config.layerMap.count(layerName) == 0) {
		throw out_of_range("ERROR: LayerAsCentroid(): a layer named as \"" + layerName + "\" doesn't exist.");
	}
	OutputObject oo(CENTROID,
					config.layerMap[layerName],
					osmID);
	outputs.push_back(oo);
}

// Set attributes in a vector tile's Attributes table
void OSMObject::Attribute(const string &key, const string &val) {
	if (val.size()==0) { return; }		// don't set empty strings
	if (outputs.size()==0) { cerr << "Can't add Attribute " << key << " if no Layer set" << endl; return; }
	vector_tile::Tile_Value v;
	v.set_string_value(val);
	outputs[outputs.size()-1].addAttribute(key, v);
	setVectorLayerMetadata(outputs[outputs.size()-1].layer, key, 0);
}

void OSMObject::AttributeNumeric(const string &key, const float val) {
	if (outputs.size()==0) { cerr << "Can't add Attribute " << key << " if no Layer set" << endl; return; }
	vector_tile::Tile_Value v;
	v.set_float_value(val);
	outputs[outputs.size()-1].addAttribute(key, v);
	setVectorLayerMetadata(outputs[outputs.size()-1].layer, key, 1);
}

void OSMObject::AttributeBoolean(const string &key, const bool val) {
	if (outputs.size()==0) { cerr << "Can't add Attribute " << key << " if no Layer set" << endl; return; }
	vector_tile::Tile_Value v;
	v.set_bool_value(val);
	outputs[outputs.size()-1].addAttribute(key, v);
	setVectorLayerMetadata(outputs[outputs.size()-1].layer, key, 2);
}

// Record attribute name/type for vector_layers table
void OSMObject::setVectorLayerMetadata(const uint_least8_t layer, const string &key, const uint type) {
	config.layers[layer].attributeMap[key] = type;
}

std::string OSMObject::serialiseLayerJSON() {
	Document document;
	document.SetObject();
	Document::AllocatorType& allocator = document.GetAllocator();

	Value layerArray(kArrayType);
	for (auto it = config.layers.begin(); it != config.layers.end(); ++it) {
		Value fieldObj(kObjectType);
		for (auto jt = it->attributeMap.begin(); jt != it->attributeMap.end(); ++jt) {
			Value k(jt->first.c_str(), allocator);
			switch (jt->second) {
				case 0: fieldObj.AddMember(k, "String" , allocator); break;
				case 1:	fieldObj.AddMember(k, "Number" , allocator); break;
				case 2:	fieldObj.AddMember(k, "Boolean", allocator); break;
			}
		}
		Value layerObj(kObjectType);
		Value name(it->name.c_str(), allocator);
		layerObj.AddMember("id",      name,        allocator);
		layerObj.AddMember("fields",  fieldObj,    allocator);
		layerObj.AddMember("minzoom", it->minzoom, allocator);
		layerObj.AddMember("maxzoom", it->maxzoom, allocator);
		layerArray.PushBack(layerObj, allocator);
	}

	document.AddMember("vector_layers", layerArray, allocator);

	StringBuffer buffer;
	Writer<StringBuffer> writer(buffer);
	document.Accept(writer);
	string json(buffer.GetString(), buffer.GetSize());
	return json;
}
