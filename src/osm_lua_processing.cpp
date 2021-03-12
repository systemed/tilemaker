#include "osm_lua_processing.h"
#include "helpers.h"
#include <iostream>
using namespace std;

kaguya::State *g_luaState = nullptr;
bool supportsRemappingShapefiles = false;

int lua_error_handler(int errCode, const char *errMessage)
{
	cerr << "lua runtime error: " << errMessage << endl;
	std::string traceback = (*g_luaState)["debug"]["traceback"];
	cerr << "traceback: " << traceback << endl;
	exit(0);
}

// ----	initialization routines

OsmLuaProcessing::OsmLuaProcessing(
    OSMStore const *indexStore, OSMStore &osmStore,
    const class Config &configIn, class LayerDefinition &layers,
	const string &luaFile,
	const class ShpMemTiles &shpMemTiles, 
	class OsmMemTiles &osmMemTiles,
	AttributeStore &attributeStore):
	indexStore(indexStore), osmStore(osmStore),
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
	if (isRelation) return true; // TODO: check it when non-multipolygon are supported
	return isClosed;
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
			auto const &relation = indexStore->retrieve<RelationStore::relation_entry_t>(relationHandle);	
			linestringCache = OSMStore::wayListLinestring(indexStore->wayListMultiPolygon(relation.first.cbegin(), relation.first.cend(), relation.second.cbegin(), relation.second.cend()));
		} else if (isWay) {
			auto const &nodeVecPtr = &indexStore->retrieve<WayStore::nodeid_vector_t>(nodeVecHandle);
			linestringCache = indexStore->nodeListLinestring(nodeVecPtr->cbegin(),nodeVecPtr->cend());
		}
	}
	return linestringCache;
}

const Polygon &OsmLuaProcessing::polygonCached() {
	if (!polygonInited) {
		polygonInited = true;
		auto const &nodeVecPtr = &indexStore->retrieve<WayStore::nodeid_vector_t>(nodeVecHandle);
		polygonCache = indexStore->nodeListPolygon(nodeVecPtr->cbegin(), nodeVecPtr->cend());
	}
	return polygonCache;
}

const MultiPolygon &OsmLuaProcessing::multiPolygonCached() {
	if (!multiPolygonInited) {
		multiPolygonInited = true;
		auto const &relation = indexStore->retrieve<RelationStore::relation_entry_t>(relationHandle);	
		multiPolygonCache = indexStore->wayListMultiPolygon(relation.first.cbegin(), relation.first.cend(), relation.second.cbegin(), relation.second.cend());
	}
	return multiPolygonCache;
}

// ----	Requests from Lua to write this way/node to a vector tile's Layer

// Add object to specified layer from Lua
void OsmLuaProcessing::Layer(const string &layerName, bool area) {
	if (layers.layerMap.count(layerName) == 0) {
		throw out_of_range("ERROR: Layer(): a layer named as \"" + layerName + "\" doesn't exist.");
	}

	OutputGeometryType geomType = isWay ? (area ? OutputGeometryType::POLYGON : OutputGeometryType::LINESTRING) : OutputGeometryType::POINT;
	try {
		if (geomType==OutputGeometryType::POINT) {
			LatpLon pt = indexStore->nodes_at(osmID);
			Point p = Point(pt.lon, pt.latp);

            CorrectGeometry(p);

        	OutputObjectRef oo(new OutputObjectOsmStorePoint(geomType, false,
	    	    			layers.layerMap[layerName],
		    	    		osmID, osmStore.store_point(osmStore.osm(), p), attributeStore.empty_set()));
    	    outputs.push_back(std::make_pair(oo, AttributeStore::key_value_set_entry_t()));
            return;
		}
		else if (geomType==OutputGeometryType::POLYGON) {
			// polygon

			MultiPolygon mp;

			if (isRelation) {
				try {
					mp = multiPolygonCached();
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

				mp.push_back(p);
			}

            CorrectGeometry(mp);

            OutputObjectRef oo(new OutputObjectOsmStoreMultiPolygon(geomType, false,
                            layers.layerMap[layerName],
                            osmID, osmStore.store_multi_polygon(osmStore.osm(), mp), attributeStore.empty_set()));
    	    outputs.push_back(std::make_pair(oo, AttributeStore::key_value_set_entry_t()));
		}
		else if (geomType==OutputGeometryType::LINESTRING) {
			// linestring
			Linestring ls = linestringCached();

            CorrectGeometry(ls);

    	    OutputObjectRef oo(new OutputObjectOsmStoreLinestring(geomType, false,
		    			layers.layerMap[layerName],
			    		osmID, osmStore.store_linestring(osmStore.osm(), ls), attributeStore.empty_set()));
    	    outputs.push_back(std::make_pair(oo, AttributeStore::key_value_set_entry_t()));
		}
	} catch (std::invalid_argument &err) {
		cerr << "Error in OutputObjectOsmStore constructor: " << err.what() << endl;
	}
}

void OsmLuaProcessing::LayerAsCentroid(const string &layerName) {
	if (layers.layerMap.count(layerName) == 0) {
		throw out_of_range("ERROR: LayerAsCentroid(): a layer named as \"" + layerName + "\" doesn't exist.");
	}

    Point geomp;
	try {

		Geometry tmp;
		if (isRelation) {
			try {
				auto const &relation = indexStore->retrieve<RelationStore::relation_entry_t>(relationHandle);	
				tmp = indexStore->wayListMultiPolygon(relation.first.cbegin(), relation.first.cend(), relation.second.cbegin(), relation.second.cend());
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
		} else {
			LatpLon pt = indexStore->nodes_at(osmID);
			tmp = Point(pt.lon, pt.latp);
		}

		if(geom::is_empty(tmp)) {
			cerr << "Geometry is empty in OsmLuaProcessing::LayerAsCentroid (" << (isRelation ? "relation " : isWay ? "way " : "node ") << originalOsmID << ")" << endl;
			return;
		}

		// write out centroid only
		try {
			Point p;
			geom::centroid(tmp, p);
			geomp = p;
		} catch (geom::centroid_exception &err) {
			cerr << "Problem geom: " << boost::geometry::wkt(tmp) << std::endl;
			cerr << err.what() << endl;
			return;
		}

	} catch (std::invalid_argument &err) {
		cerr << "Error in OutputObjectOsmStore constructor: " << err.what() << endl;
	}

	OutputObjectRef oo(new OutputObjectOsmStorePoint(OutputGeometryType::POINT,
					false, layers.layerMap[layerName],
					osmID, osmStore.store_point(osmStore.osm(), geomp), attributeStore.empty_set()));
    outputs.push_back(std::make_pair(oo, AttributeStore::key_value_set_entry_t()));
}

// Set attributes in a vector tile's Attributes table
void OsmLuaProcessing::Attribute(const string &key, const string &val) {
	if (val.size()==0) { return; }		// don't set empty strings
	if (outputs.size()==0) { cerr << "Can't add Attribute " << key << " if no Layer set" << endl; return; }
	vector_tile::Tile_Value v;
	v.set_string_value(val);
	outputs.back().second.push_back(attributeStore.store_key_value(key, v));
	setVectorLayerMetadata(outputs.back().first->layer, key, 0);
}

void OsmLuaProcessing::AttributeNumeric(const string &key, const float val) {
	if (outputs.size()==0) { cerr << "Can't add Attribute " << key << " if no Layer set" << endl; return; }
	vector_tile::Tile_Value v;
	v.set_float_value(val);
	outputs.back().second.push_back(attributeStore.store_key_value(key, v));
	setVectorLayerMetadata(outputs.back().first->layer, key, 1);
}

void OsmLuaProcessing::AttributeBoolean(const string &key, const bool val) {
	if (outputs.size()==0) { cerr << "Can't add Attribute " << key << " if no Layer set" << endl; return; }
	vector_tile::Tile_Value v;
	v.set_bool_value(val);
	outputs.back().second.push_back(attributeStore.store_key_value(key, v));
	setVectorLayerMetadata(outputs.back().first->layer, key, 2);
}

// Set minimum zoom
void OsmLuaProcessing::MinZoom(const unsigned z) {
	if (outputs.size()==0) { cerr << "Can't set minimum zoom if no Layer set" << endl; return; }
	outputs.back().first->setMinZoom(z);
}

// Record attribute name/type for vector_layers table
void OsmLuaProcessing::setVectorLayerMetadata(const uint_least8_t layer, const string &key, const uint type) {
	layers.layers[layer].attributeMap[key] = type;
}

// We are now processing a node
void OsmLuaProcessing::setNode(NodeID id, LatpLon node, const tag_map_t &tags) {
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
			
			// Store the attributes of the generated geometry
			jt->first->setAttributeSet(attributeStore.store_set(jt->second));		

			osmMemTiles.AddObject(index, jt->first);
		}
	}
}

// We are now processing a way
void OsmLuaProcessing::setWay(WayID wayId, OSMStore::handle_t handle, const tag_map_t &tags) {
	reset();
	osmID = wayId;
	originalOsmID = osmID;
	isWay = true;
	isRelation = false;
	nodeVecHandle = handle;
	relationHandle = OSMStore::handle_t();
	linestringInited = polygonInited = multiPolygonInited = false;

	try {
		auto const &nodeVecPtr = &indexStore->retrieve<WayStore::nodeid_vector_t>(nodeVecHandle);
		isClosed = nodeVecPtr->front()==nodeVecPtr->back();
		setLocation(indexStore->nodes_at(nodeVecPtr->front()).lon, indexStore->nodes_at(nodeVecPtr->front()).latp,
				indexStore->nodes_at(nodeVecPtr->back()).lon, indexStore->nodes_at(nodeVecPtr->back()).latp);

	} catch (std::out_of_range &err) {
		std::stringstream ss;
		ss << "Way " << originalOsmID << " is missing a node";
		throw std::out_of_range(ss.str());
	}

	currentTags = tags;
	/*currentTags.clear();
	for(auto const &i: tags) {
		currentTags.emplace(std::piecewise_construct,
			std::forward_as_tuple(i.first.begin(), i.first.end()), 
			std::forward_as_tuple(i.second.begin(), i.second.end()));
	} */


	bool ok = true;
	if (ok) {
		luaState.setErrorHandler(kaguya::ErrorHandler::throwDefaultError);

		//Start Lua processing for way
		kaguya::LuaFunction way_function = luaState["way_function"];
		kaguya::LuaRef ret = way_function(this);
		assert(!ret);
	}

	if (!this->empty()) {
		// create a list of tiles this way passes through (tileSet)
		unordered_set<TileCoordinates> tileSet;
		try {
			auto const &nodeVecPtr = &indexStore->retrieve<WayStore::nodeid_vector_t>(nodeVecHandle);
			insertIntermediateTiles(indexStore->nodeListLinestring(nodeVecPtr->cbegin(),nodeVecPtr->cend()), this->config.baseZoom, tileSet);

			// then, for each tile, store the OutputObject for each layer
			bool polygonExists = false;
			for (auto it = tileSet.begin(); it != tileSet.end(); ++it) {
				TileCoordinates index = *it;
				for (auto jt = this->outputs.begin(); jt != this->outputs.end(); ++jt) {

					// Store the attributes of the generated geometry
					jt->first->setAttributeSet(attributeStore.store_set(jt->second));		

					if (jt->first->geomType == OutputGeometryType::POLYGON) {
						polygonExists = true;
						continue;
					}
					osmMemTiles.AddObject(index, jt->first);
				}
			}

			// for polygon, fill inner tiles
			if (polygonExists) {
				fillCoveredTiles(tileSet);
				for (auto it = tileSet.begin(); it != tileSet.end(); ++it) {
					TileCoordinates index = *it;
					for (auto jt = this->outputs.begin(); jt != this->outputs.end(); ++jt) {

						// Store the attributes of the generated geometry
						jt->first->setAttributeSet(attributeStore.store_set(jt->second));		

						if (jt->first->geomType != OutputGeometryType::POLYGON) continue;
						osmMemTiles.AddObject(index, jt->first);
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
void OsmLuaProcessing::setRelation(int64_t relationId, OSMStore::handle_t relationHandle, const tag_map_t &tags) {
	reset();
	osmID = --newWayID;
	originalOsmID = relationId;
	isWay = true;
	isRelation = true;

	this->relationHandle = relationHandle;
	//setLocation(...); TODO

	currentTags = tags;
	/* currentTags.clear();
	for(auto const &i: tags) {
		currentTags.emplace(std::piecewise_construct,
			std::forward_as_tuple(i.first.begin(), i.first.end()), 
			std::forward_as_tuple(i.second.begin(), i.second.end()));
	} */

	bool ok = true;
	if (ok) {
		//Start Lua processing for relation
		luaState["way_function"](this);
	}

	if (!this->empty()) {								
		MultiPolygon mp;
		try {
			// for each tile the relation may cover, put the output objects.
			auto const &relation = indexStore->retrieve<RelationStore::relation_entry_t>(relationHandle);	
			mp = indexStore->wayListMultiPolygon(relation.first.cbegin(), relation.first.cend(), relation.second.cbegin(), relation.second.cend());
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
				// Store the attributes of the generated geometry
				jt->first->setAttributeSet(attributeStore.store_set(jt->second));		

				osmMemTiles.AddObject(index, jt->first);
			}
		}
	}
}

vector<string> OsmLuaProcessing::GetSignificantNodeKeys() {
	return luaState["node_keys"];
}

