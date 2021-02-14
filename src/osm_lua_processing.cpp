#include "osm_lua_processing.h"
#include "helpers.h"
#include <iostream>
using namespace std;

kaguya::State *g_luaState = nullptr;
bool supportsRemappingShapefiles = false;
extern bool verbose;

int lua_error_handler(int errCode, const char *errMessage)
{
	cerr << "lua runtime error: " << errMessage << endl;
	std::string traceback = (*g_luaState)["debug"]["traceback"];
	cerr << "traceback: " << traceback << endl;
	exit(0);
}

// ----	initialization routines

OsmLuaProcessing::OsmLuaProcessing(const class Config &configIn, class LayerDefinition &layers,
	const string &luaFile,
	const string &osmStoreFilename,
	const class ShpMemTiles &shpMemTiles, 
	class OsmMemTiles &osmMemTiles,
	AttributeStore &attributeStore):
	osmStore(osmStoreFilename),
	shpMemTiles(shpMemTiles),
	osmMemTiles(osmMemTiles),
	attributeStore(attributeStore),
	config(configIn),
	layers(layers) {

	newWayID = MAX_WAY_ID;

	// ----	Initialise Lua
	g_luaState = &luaState;
	luaState.setErrorHandler(lua_error_handler);
	luaState.dofile(luaFile.c_str());
	luaState["OSM"].setClass(kaguya::UserdataMetatable<OsmLuaProcessing>()
		.addFunction("Id", &OsmLuaProcessing::Id)
		.addFunction("Holds", &OsmLuaProcessing::Holds)
		.addFunction("Find", &OsmLuaProcessing::Find)
		.addFunction("FindIntersecting", &OsmLuaProcessing::FindIntersecting)
		.addFunction("Intersects", &OsmLuaProcessing::Intersects)
		.addFunction("IsClosed", &OsmLuaProcessing::IsClosed)
		.addFunction("Area", &OsmLuaProcessing::Area)
		.addFunction("Length", &OsmLuaProcessing::Length)
		.addFunction("Layer", &OsmLuaProcessing::Layer)
		.addFunction("LayerAsCentroid", &OsmLuaProcessing::LayerAsCentroid)
		.addFunction("Attribute", &OsmLuaProcessing::Attribute)
		.addFunction("AttributeNumeric", &OsmLuaProcessing::AttributeNumeric)
		.addFunction("AttributeBoolean", &OsmLuaProcessing::AttributeBoolean)
		.addFunction("MinZoom", &OsmLuaProcessing::MinZoom)
	);
	if (luaState["attribute_function"]) {
		supportsRemappingShapefiles = true;
	} else {
		supportsRemappingShapefiles = false;
	}

	// ---- Call init_function of Lua logic

	luaState("if init_function~=nil then init_function() end");

}

OsmLuaProcessing::~OsmLuaProcessing() {
	// Call exit_function of Lua logic
	luaState("if exit_function~=nil then exit_function() end");
}

// ----	Helpers provided for main routine

// Has this object been assigned to any layers?
bool OsmLuaProcessing::empty() {
	return outputs.size()==0;
}

bool OsmLuaProcessing::canRemapShapefiles() {
	return supportsRemappingShapefiles;
}

kaguya::LuaTable OsmLuaProcessing::newTable() {
	return luaState.newTable();//kaguya::LuaTable(luaState);
}

kaguya::LuaTable OsmLuaProcessing::remapAttributes(kaguya::LuaTable& in_table) {
	kaguya::LuaTable out_table = luaState["attribute_function"].call<kaguya::LuaTable>(in_table);
	return out_table;
}

// ----	Metadata queries called from Lua

// Get the ID of the current object
string OsmLuaProcessing::Id() const {
	return to_string(originalOsmID);
}

// Check if there's a value for a given key
bool OsmLuaProcessing::Holds(const string& key) const {
	return currentTags.find(key) != currentTags.end();
}

// Get an OSM tag for a given key (or return empty string if none)
string OsmLuaProcessing::Find(const string& key) const {
	auto it = currentTags.find(key);
	if(it == currentTags.end()) return "";
	return it->second;
}

// ----	Spatial queries called from Lua

// Find intersecting shapefile layer
vector<string> OsmLuaProcessing::FindIntersecting(const string &layerName) {
	// TODO: multipolygon relations not supported, will always return empty vector
	if(isRelation) return vector<string>();
	Point p1(lon1/10000000.0,latp1/10000000.0);
	Point p2(lon2/10000000.0,latp2/10000000.0);
	Box box = Box(p1,p2);
	return shpMemTiles.FindIntersecting(layerName, box);
}

bool OsmLuaProcessing::Intersects(const string &layerName) {
	// TODO: multipolygon relations not supported, will always return false
	if(isRelation) return false;
	Point p1(lon1/10000000.0,latp1/10000000.0);
	Point p2(lon2/10000000.0,latp2/10000000.0);
	Box box = Box(p1,p2);
	return shpMemTiles.Intersects(layerName, box);
}

// Returns whether it is closed polygon
bool OsmLuaProcessing::IsClosed() const {
	if (!isWay) return false; // nonsense: it isn't a way
	if (isRelation) {
		return true; // TODO: check it when non-multipolygon are supported
	} else {
		return nodeVec->front() == nodeVec->back();
	}
}

void reverse_project(DegPoint& p) {
    geom::set<1>(p, latp2lat(geom::get<1>(p)));
}

// Returns area
double OsmLuaProcessing::Area() {
	if (!IsClosed()) return 0;

#if BOOST_VERSION >= 106700
	geom::strategy::area::spherical<> sph_strategy(RadiusMeter);
	if (isRelation) {
		// Boost won't calculate area of a multipolygon, so we just total up the member polygons
		double totalArea = 0;
		MultiPolygon mp = multiPolygonCached();
		for (MultiPolygon::const_iterator it = mp.begin(); it != mp.end(); ++it) {
			geom::model::polygon<DegPoint> p;
			geom::assign(p,*it);
			geom::for_each_point(p, reverse_project);
			double area = geom::area(p, sph_strategy);
			totalArea += area;
		}
		return totalArea;
	} else if (isWay) {
		// Reproject back into lat/lon and then run Boo
		geom::model::polygon<DegPoint> p;
		geom::assign(p,polygonCached());
		geom::for_each_point(p, reverse_project);
		return geom::area(p, sph_strategy);
	}
#else
	if (isRelation) {
		return geom::area(multiPolygonCached());
	} else if (isWay) {
		return geom::area(polygonCached());
	}
#endif

	return 0;
}

// Returns length
double OsmLuaProcessing::Length() {
	if (isWay) {
		geom::model::linestring<DegPoint> l;
		geom::assign(l, linestringCached());
		geom::for_each_point(l, reverse_project);
		return geom::length(l, geom::strategy::distance::haversine<float>(RadiusMeter));
	}
	// multi_polygon would be calculated as zero
	return 0;
}

// Cached geometries creation
const Linestring &OsmLuaProcessing::linestringCached() {
	if (!linestringInited) {
		linestringInited = true;

		if (isRelation) {
			//A relation is being treated as a linestring, which might be
			//caused by bug in the Lua script
			linestringCache = osmStore.wayListLinestring(*outerWayVec, *innerWayVec);
		} else if (isWay) {
			linestringCache = osmStore.nodeListLinestring(*nodeVec);
		}
	}
	return linestringCache;
}

const Polygon &OsmLuaProcessing::polygonCached() {
	if (!polygonInited) {
		polygonInited = true;
		polygonCache = osmStore.nodeListPolygon(*nodeVec);
	}
	return polygonCache;
}

const MultiPolygon &OsmLuaProcessing::multiPolygonCached() {
	if (!multiPolygonInited) {
		multiPolygonInited = true;
		multiPolygonCache = osmStore.wayListMultiPolygon(*outerWayVec, *innerWayVec);
	}
	return multiPolygonCache;
}

// ----	Requests from Lua to write this way/node to a vector tile's Layer

// Add object to specified layer from Lua
void OsmLuaProcessing::Layer(const string &layerName, bool area) {
	if (layers.layerMap.count(layerName) == 0) {
		throw out_of_range("ERROR: Layer(): a layer named as \"" + layerName + "\" doesn't exist.");
	}

	Geometry geom;
	OutputGeometryType geomType = isWay ? (area ? POLYGON : LINESTRING) : POINT;
	try {
		if (geomType==POINT) {
			LatpLon pt = osmStore.nodes_at(osmID);
			geom = Point(pt.lon, pt.latp);
		}
		else if (geomType==POLYGON) {
			// polygon

			if (isRelation) {
				try {
					geom = multiPolygonCached();
				} catch(std::out_of_range &err) {
					cout << "In relation " << originalOsmID << ": " << err.what() << endl;
					return;
				}
			}
			else if (isWay) {
				//Is there a more efficient way to do this?
				Linestring ls = linestringCached();
				Polygon p;
				geom::assign_points(p, ls);
				MultiPolygon mp;
				mp.push_back(p);
				geom = mp;
			}

		}
		else if (geomType==LINESTRING) {
			// linestring
			geom = linestringCached();
		}
	} catch (std::invalid_argument &err) {
		cerr << "Error in OutputObjectOsmStore constructor: " << err.what() << endl;
	}

	geom::correct(geom); // fix wrong orientation
#if BOOST_VERSION >= 105800
	geom::validity_failure_type failure;
	if (isRelation && !geom::is_valid(geom,failure)) {
		if (verbose) cout << "Relation " << originalOsmID << " has " << boost_validity_error(failure) << endl;
		if (failure==10) return; // too few points
	} else if (isWay && !geom::is_valid(geom,failure)) {
		if (verbose) cout << "Way " << originalOsmID << " has " << boost_validity_error(failure) << endl;
		if (failure==10) return; // too few points
	}
#endif

	OutputObjectRef oo = std::make_shared<OutputObjectOsmStore>(geomType,
					layers.layerMap[layerName],
					osmID, geom);
	outputs.push_back(oo);
}

void OsmLuaProcessing::LayerAsCentroid(const string &layerName) {
	if (layers.layerMap.count(layerName) == 0) {
		throw out_of_range("ERROR: LayerAsCentroid(): a layer named as \"" + layerName + "\" doesn't exist.");
	}

	Geometry geom;
	try {

		Geometry tmp;
		if (isRelation) {
			try {
				tmp = osmStore.wayListMultiPolygon(*outerWayVec, *innerWayVec);
			} catch(std::out_of_range &err) {
				cout << "In relation " << originalOsmID << ": " << err.what() << endl;
				return;
			}
		} else if (isWay) {
			//Is there a more efficient way to do this?
			Linestring ls = linestringCached();
			Polygon p;
			geom::assign_points(p, ls);
			MultiPolygon mp;
			mp.push_back(p);	
			tmp = mp;
		}

#if BOOST_VERSION >= 105900
		if(geom::is_empty(tmp)) {
			cerr << "Geometry is empty in OsmLuaProcessing::LayerAsCentroid" << endl;
			return;
		}
#endif

		// write out centroid only
		try {
			Point p;
			geom::centroid(tmp, p);
			geom = p;
		} catch (geom::centroid_exception &err) {
			cerr << "Problem geom: " << boost::geometry::wkt(tmp) << std::endl;
			cerr << err.what() << endl;
			return;
		}

	} catch (std::invalid_argument &err) {
		cerr << "Error in OutputObjectOsmStore constructor: " << err.what() << endl;
	}

	OutputObjectRef oo = std::make_shared<OutputObjectOsmStore>(CENTROID,
					layers.layerMap[layerName],
					osmID, geom);
	outputs.push_back(oo);
}

// Set attributes in a vector tile's Attributes table
void OsmLuaProcessing::Attribute(const string &key, const string &val) {
	if (val.size()==0) { return; }		// don't set empty strings
	if (outputs.size()==0) { cerr << "Can't add Attribute " << key << " if no Layer set" << endl; return; }
	vector_tile::Tile_Value v;
	v.set_string_value(val);
	unsigned attrIndex = attributeStore.indexForPair(key,v,false);
	outputs[outputs.size()-1]->addAttribute(attrIndex);
	setVectorLayerMetadata(outputs[outputs.size()-1]->layer, key, 0);
}

void OsmLuaProcessing::AttributeNumeric(const string &key, const float val) {
	if (outputs.size()==0) { cerr << "Can't add Attribute " << key << " if no Layer set" << endl; return; }
	vector_tile::Tile_Value v;
	v.set_float_value(val);
	unsigned attrIndex = attributeStore.indexForPair(key,v,false);
	outputs[outputs.size()-1]->addAttribute(attrIndex);
	setVectorLayerMetadata(outputs[outputs.size()-1]->layer, key, 1);
}

void OsmLuaProcessing::AttributeBoolean(const string &key, const bool val) {
	if (outputs.size()==0) { cerr << "Can't add Attribute " << key << " if no Layer set" << endl; return; }
	vector_tile::Tile_Value v;
	v.set_bool_value(val);
	unsigned attrIndex = attributeStore.indexForPair(key,v,false);
	outputs[outputs.size()-1]->addAttribute(attrIndex);
	setVectorLayerMetadata(outputs[outputs.size()-1]->layer, key, 2);
}

// Set minimum zoom
void OsmLuaProcessing::MinZoom(const unsigned z) {
	if (outputs.size()==0) { cerr << "Can't set minimum zoom if no Layer set" << endl; return; }
	outputs[outputs.size()-1]->setMinZoom(z);
}

// Record attribute name/type for vector_layers table
void OsmLuaProcessing::setVectorLayerMetadata(const uint_least8_t layer, const string &key, const uint type) {
	layers.layers[layer].attributeMap[key] = type;
}

void OsmLuaProcessing::startOsmData() {
	osmStore.clear();
}

void OsmLuaProcessing::everyNode(NodeID id, LatpLon node) {
	osmStore.nodes_insert_back(id, node);
}

// We are now processing a node
void OsmLuaProcessing::setNode(NodeID id, LatpLon node, const std::map<std::string, std::string> &tags) {
	reset();
	osmID = id;
	originalOsmID = id;
	isWay = false;
	isRelation = false;

	setLocation(node.lon, node.latp, node.lon, node.latp);

	currentTags = tags;

	//Start Lua processing for node
	luaState["node_function"](this);
	if (!this->empty()) {
		TileCoordinates index = latpLon2index(node, this->config.baseZoom);
		for (auto jt = this->outputs.begin(); jt != this->outputs.end(); ++jt) {
			osmMemTiles.AddObject(index, *jt);
		}
	}
}

// We are now processing a way
void OsmLuaProcessing::setWay(Way *way, NodeVec *nodeVecPtr, bool inRelation, const std::map<std::string, std::string> &tags) {
	reset();
	osmID = way->id();
	originalOsmID = osmID;
	isWay = true;
	isRelation = false;

	outerWayVec = nullptr;
	innerWayVec = nullptr;
	nodeVec = nodeVecPtr;
	try {
		setLocation(osmStore.nodes_at(nodeVec->front()).lon, osmStore.nodes_at(nodeVec->front()).latp,
				osmStore.nodes_at(nodeVec->back()).lon, osmStore.nodes_at(nodeVec->back()).latp);

	} catch (std::out_of_range &err) {
		std::stringstream ss;
		ss << "Way " << originalOsmID << " is missing a node";
		throw std::out_of_range(ss.str());
	}

	currentTags = tags;

	bool ok = true;
	if (ok) {
		luaState.setErrorHandler(kaguya::ErrorHandler::throwDefaultError);

		//Start Lua processing for way
		kaguya::LuaFunction way_function = luaState["way_function"];
		kaguya::LuaRef ret = way_function(this);
		assert(!ret);
	}

	if (!this->empty() || inRelation) {
		// Store the way's nodes in the global way store
		WayID wayId = static_cast<WayID>(way->id());
		osmStore.ways_insert_back(wayId, *nodeVec);
	}

	if (!this->empty()) {
		// create a list of tiles this way passes through (tileSet)
		unordered_set<TileCoordinates> tileSet;
		try {
			insertIntermediateTiles(osmStore.nodeListLinestring(*nodeVec), this->config.baseZoom, tileSet);

			// then, for each tile, store the OutputObject for each layer
			bool polygonExists = false;
			for (auto it = tileSet.begin(); it != tileSet.end(); ++it) {
				TileCoordinates index = *it;
				for (auto jt = this->outputs.begin(); jt != this->outputs.end(); ++jt) {
					if ((*jt)->geomType == POLYGON) {
						polygonExists = true;
						continue;
					}
					osmMemTiles.AddObject(index, *jt);
				}
			}

			// for polygon, fill inner tiles
			if (polygonExists) {
				fillCoveredTiles(tileSet);
				for (auto it = tileSet.begin(); it != tileSet.end(); ++it) {
					TileCoordinates index = *it;
					for (auto jt = this->outputs.begin(); jt != this->outputs.end(); ++jt) {
						if ((*jt)->geomType != POLYGON) continue;
						osmMemTiles.AddObject(index, *jt);
					}
				}
			}
		} catch(std::out_of_range &err) {
			cerr << "Error calculating intermediate tiles: " << err.what() << endl;
		}
	}
}

// We are now processing a relation
// (note that we store relations as ways with artificial IDs, and that
//  we use decrementing positive IDs to give a bit more space for way IDs)
void OsmLuaProcessing::setRelation(Relation *relation, WayVec *outerWayVecPtr, WayVec *innerWayVecPtr,
	const std::map<std::string, std::string> &tags) {
	reset();
	osmID = --newWayID;
	originalOsmID = relation->id();
	isWay = true;
	isRelation = true;

	outerWayVec = outerWayVecPtr;
	innerWayVec = innerWayVecPtr;
	nodeVec = nullptr;
	//setLocation(...); TODO

	currentTags = tags;

	bool ok = true;
	if (ok) {
		//Start Lua processing for relation
		luaState["way_function"](this);
	}

	if (!this->empty()) {								

		WayID relID = this->osmID;
		// Store the relation members in the global relation store
		osmStore.relations_insert_front(relID, *outerWayVec, *innerWayVec);

		MultiPolygon mp;
		try {
			// for each tile the relation may cover, put the output objects.
			mp = osmStore.wayListMultiPolygon(*outerWayVec, *innerWayVec);
		} catch(std::out_of_range &err) {
			cout << "In relation " << originalOsmID << ": " << err.what() << endl;
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
				osmMemTiles.AddObject(index, *jt);
			}
		}
	}
}


void OsmLuaProcessing::endOsmData() {
	osmStore.reportSize();
}

vector<string> OsmLuaProcessing::GetSignificantNodeKeys() {
	return luaState["node_keys"];
}

