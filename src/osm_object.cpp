#include "osm_object.h"
using namespace std;
using namespace rapidjson;

// ----	initialization routines

OSMObject::OSMObject(const class Config &configIn, class LayerDefinition &layers,
	kaguya::State &luaObj, 
	vector<Geometry> &geomPtr, map<uint,string> &namePtr, OSMStore *storePtr,
	TileIndex &tileIndex):
	luaState(luaObj),
	cachedGeometries(geomPtr),
	cachedGeometryNames(namePtr),
	tileIndex(tileIndex),
	config(configIn),
	layers(layers)
{
	newWayID = MAX_WAY_ID;
	osmStore = storePtr;
}

// ----	Helpers provided for main routine

// Has this object been assigned to any layers?
bool OSMObject::empty() {
	return outputs.size()==0;
}

// ----	Metadata queries called from Lua

// Get the ID of the current object
string OSMObject::Id() const {
	return to_string(osmID);
}

// Check if there's a value for a given key
bool OSMObject::Holds(const string& key) const {
	
	return currentTags.find(key) != currentTags.end();
}

// Get an OSM tag for a given key (or return empty string if none)
string OSMObject::Find(const string& key) const {

	auto it = currentTags.find(key);
	if(it == currentTags.end()) return "";
	return it->second;
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
	if (layers.layerMap.count(layerName) == 0) {
		throw out_of_range("ERROR: Layer(): a layer named as \"" + layerName + "\" doesn't exist.");
	}
	std::shared_ptr<OutputObject> oo = std::make_shared<OutputObjectOsmStore>(isWay ? (area ? POLYGON : LINESTRING) : POINT,
					layers.layerMap[layerName],
					osmID);
	outputs.push_back(oo);
}
void OSMObject::LayerAsCentroid(const string &layerName) {
	if (layers.layerMap.count(layerName) == 0) {
		throw out_of_range("ERROR: LayerAsCentroid(): a layer named as \"" + layerName + "\" doesn't exist.");
	}
	std::shared_ptr<OutputObject> oo = std::make_shared<OutputObjectOsmStore>(CENTROID,
					layers.layerMap[layerName],
					osmID);
	outputs.push_back(oo);
}

// Set attributes in a vector tile's Attributes table
void OSMObject::Attribute(const string &key, const string &val) {
	if (val.size()==0) { return; }		// don't set empty strings
	if (outputs.size()==0) { cerr << "Can't add Attribute " << key << " if no Layer set" << endl; return; }
	vector_tile::Tile_Value v;
	v.set_string_value(val);
	outputs[outputs.size()-1]->addAttribute(key, v);
	setVectorLayerMetadata(outputs[outputs.size()-1]->layer, key, 0);
}

void OSMObject::AttributeNumeric(const string &key, const float val) {
	if (outputs.size()==0) { cerr << "Can't add Attribute " << key << " if no Layer set" << endl; return; }
	vector_tile::Tile_Value v;
	v.set_float_value(val);
	outputs[outputs.size()-1]->addAttribute(key, v);
	setVectorLayerMetadata(outputs[outputs.size()-1]->layer, key, 1);
}

void OSMObject::AttributeBoolean(const string &key, const bool val) {
	if (outputs.size()==0) { cerr << "Can't add Attribute " << key << " if no Layer set" << endl; return; }
	vector_tile::Tile_Value v;
	v.set_bool_value(val);
	outputs[outputs.size()-1]->addAttribute(key, v);
	setVectorLayerMetadata(outputs[outputs.size()-1]->layer, key, 2);
}

// Record attribute name/type for vector_layers table
void OSMObject::setVectorLayerMetadata(const uint_least8_t layer, const string &key, const uint type) {
	layers.layers[layer].attributeMap[key] = type;
}

std::string OSMObject::serialiseLayerJSON() {
	Document document;
	document.SetObject();
	Document::AllocatorType& allocator = document.GetAllocator();

	Value layerArray(kArrayType);
	for (auto it = layers.layers.begin(); it != layers.layers.end(); ++it) {
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

void OSMObject::everyNode(NodeID id, LatpLon node)
{
	osmStore->nodes.insert_back(id, node);
}

// We are now processing a node
void OSMObject::setNode(NodeID id, LatpLon node, const std::map<std::string, std::string> &tags) {
	reset();
	osmID = id;
	isWay = false;
	isRelation = false;

	setLocation(node.lon, node.latp, node.lon, node.latp);

	currentTags = tags;

	this->luaState["node_function"](this);
	if (!this->empty()) {
		TileCoordinates index = latpLon2index(node, this->config.baseZoom);
		for (auto jt = this->outputs.begin(); jt != this->outputs.end(); ++jt) {
			tileIndex[index].push_back(*jt);
		}
	}
}

// We are now processing a way
void OSMObject::setWay(Way *way, NodeVec *nodeVecPtr, bool inRelation, const std::map<std::string, std::string> &tags) {
	reset();
	osmID = way->id();
	isWay = true;
	isRelation = false;

	nodeVec = nodeVecPtr;
	try {
		setLocation(osmStore->nodes.at(nodeVec->front()).lon, osmStore->nodes.at(nodeVec->front()).latp,
				osmStore->nodes.at(nodeVec->back()).lon, osmStore->nodes.at(nodeVec->back()).latp);

	} catch (std::out_of_range &err) {
		std::stringstream ss;
		ss << "Way " << osmID << " is missing a node";
		throw std::out_of_range(ss.str());
	}

	currentTags = tags;

	bool ok = true;
	if (ok)
	{
		this->luaState.setErrorHandler(kaguya::ErrorHandler::throwDefaultError);
		kaguya::LuaFunction way_function = this->luaState["way_function"];
		kaguya::LuaRef ret = way_function(this);
		assert(!ret);
	}

	if (!this->empty() || inRelation) {
		// Store the way's nodes in the global way store
		WayStore &ways = osmStore->ways;
		WayID wayId = static_cast<WayID>(way->id());
		ways.insert_back(wayId, *nodeVec);
	}

	if (!this->empty()) {
		// create a list of tiles this way passes through (tileSet)
		unordered_set<TileCoordinates> tileSet;
		try {
			insertIntermediateTiles(osmStore->nodeListLinestring(*nodeVec), this->config.baseZoom, tileSet);

			// then, for each tile, store the OutputObject for each layer
			bool polygonExists = false;
			for (auto it = tileSet.begin(); it != tileSet.end(); ++it) {
				TileCoordinates index = *it;
				for (auto jt = this->outputs.begin(); jt != this->outputs.end(); ++jt) {
					if ((*jt)->geomType == POLYGON) {
						polygonExists = true;
						continue;
					}
					tileIndex[index].push_back(*jt);
				}
			}

			// for polygon, fill inner tiles
			if (polygonExists) {
				fillCoveredTiles(tileSet);
				for (auto it = tileSet.begin(); it != tileSet.end(); ++it) {
					TileCoordinates index = *it;
					for (auto jt = this->outputs.begin(); jt != this->outputs.end(); ++jt) {
						if ((*jt)->geomType != POLYGON) continue;
						tileIndex[index].push_back(*jt);
					}
				}
			}
		} catch(std::out_of_range &err)
		{
			cerr << "Error calculating intermediate tiles: " << err.what() << endl;
		}
	}

}

// We are now processing a relation
// (note that we store relations as ways with artificial IDs, and that
//  we use decrementing positive IDs to give a bit more space for way IDs)
void OSMObject::setRelation(Relation *relation, WayVec *outerWayVecPtr, WayVec *innerWayVecPtr,
	const std::map<std::string, std::string> &tags) {
	reset();
	osmID = --newWayID;
	isWay = true;
	isRelation = true;

	outerWayVec = outerWayVecPtr;
	innerWayVec = innerWayVecPtr;
	//setLocation(...); TODO

	currentTags = tags;

	bool ok = true;
	if (ok)
		this->luaState["way_function"](this);

	if (!this->empty()) {								

		WayID relID = this->osmID;
		// Store the relation members in the global relation store
		RelationStore &relations = osmStore->relations;
		relations.insert_front(relID, *outerWayVec, *innerWayVec);

		MultiPolygon mp;
		try
		{
			// for each tile the relation may cover, put the output objects.
			mp = osmStore->wayListMultiPolygon(*outerWayVec, *innerWayVec);
		}
		catch(std::out_of_range &err)
		{
			cout << "In relation " << relID << ": " << err.what() << endl;
			return;
		}		

		unordered_set<TileCoordinates> tileSet;
		if (mp.size() == 1) {
			insertIntermediateTiles(mp[0].outer(), this->config.baseZoom, tileSet);
			fillCoveredTiles(tileSet);
		} else {
			for (Polygon poly: mp) {
				unordered_set<TileCoordinates> tileSetTmp;
				insertIntermediateTiles(poly.outer(), this->config.baseZoom, tileSetTmp);
				fillCoveredTiles(tileSetTmp);
				tileSet.insert(tileSetTmp.begin(), tileSetTmp.end());
			}
		}

		for (auto it = tileSet.begin(); it != tileSet.end(); ++it) {
			TileCoordinates index = *it;
			for (auto jt = this->outputs.begin(); jt != this->outputs.end(); ++jt) {
				tileIndex[index].push_back(*jt);
			}
		}
	}

}

