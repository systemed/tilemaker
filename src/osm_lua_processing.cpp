#include "osm_lua_processing.h"
#include "helpers.h"
#include <iostream>


using namespace std;

thread_local kaguya::State *g_luaState = nullptr;
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
    OSMStore &osmStore,
    const class Config &configIn, class LayerDefinition &layers,
	const string &luaFile,
	const class ShpMemTiles &shpMemTiles, 
	class OsmMemTiles &osmMemTiles,
	AttributeStore &attributeStore):
	osmStore(osmStore),
	shpMemTiles(shpMemTiles),
	osmMemTiles(osmMemTiles),
	attributeStore(attributeStore),
	config(configIn),
	layers(layers) {

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
		.addFunction("FindCovering", &OsmLuaProcessing::FindCovering)
		.addFunction("CoveredBy", &OsmLuaProcessing::CoveredBy)
		.addFunction("IsClosed", &OsmLuaProcessing::IsClosed)
		.addFunction("Area", &OsmLuaProcessing::Area)
		.addFunction("AreaIntersecting", &OsmLuaProcessing::AreaIntersecting)
		.addFunction("Length", &OsmLuaProcessing::Length)
		.addFunction("Centroid", &OsmLuaProcessing::Centroid)
		.addFunction("Layer", &OsmLuaProcessing::Layer)
		.addFunction("LayerAsCentroid", &OsmLuaProcessing::LayerAsCentroid)
		.addOverloadedFunctions("Attribute", &OsmLuaProcessing::Attribute, &OsmLuaProcessing::AttributeWithMinZoom)
		.addOverloadedFunctions("AttributeNumeric", &OsmLuaProcessing::AttributeNumeric, &OsmLuaProcessing::AttributeNumericWithMinZoom)
		.addOverloadedFunctions("AttributeBoolean", &OsmLuaProcessing::AttributeBoolean, &OsmLuaProcessing::AttributeBooleanWithMinZoom)
		.addFunction("MinZoom", &OsmLuaProcessing::MinZoom)
		.addFunction("ZOrder", &OsmLuaProcessing::ZOrder)
		.addFunction("Accept", &OsmLuaProcessing::Accept)
		.addFunction("NextRelation", &OsmLuaProcessing::NextRelation)
		.addFunction("FindInRelation", &OsmLuaProcessing::FindInRelation)
	);
	supportsRemappingShapefiles = !!luaState["attribute_function"];
	supportsReadingRelations    = !!luaState["relation_scan_function"];
	supportsWritingRelations    = !!luaState["relation_function"];

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

bool OsmLuaProcessing::canReadRelations() {
	return supportsReadingRelations;
}

bool OsmLuaProcessing::canWriteRelations() {
	return supportsWritingRelations;
}

kaguya::LuaTable OsmLuaProcessing::newTable() {
	return luaState.newTable();//kaguya::LuaTable(luaState);
}

kaguya::LuaTable OsmLuaProcessing::remapAttributes(kaguya::LuaTable& in_table, const std::string &layerName) {
	kaguya::LuaTable out_table = luaState["attribute_function"].call<kaguya::LuaTable>(in_table, layerName);
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

vector<string> OsmLuaProcessing::FindIntersecting(const string &layerName) {
	if      (!isWay   ) { return shpMemTiles.namesOfGeometries(intersectsQuery(layerName, false, getPoint())); }
	else if (!isClosed && isRelation) { return shpMemTiles.namesOfGeometries(intersectsQuery(layerName, false, multiLinestringCached())); }
	else if (!isClosed) { return shpMemTiles.namesOfGeometries(intersectsQuery(layerName, false, linestringCached())); }
	else if (isRelation){ return shpMemTiles.namesOfGeometries(intersectsQuery(layerName, false, multiPolygonCached())); }
	else                { return shpMemTiles.namesOfGeometries(intersectsQuery(layerName, false, polygonCached())); }
}

bool OsmLuaProcessing::Intersects(const string &layerName) {
	if      (!isWay   ) { return !intersectsQuery(layerName, true, getPoint()).empty(); }
	else if (!isClosed) { return !intersectsQuery(layerName, true, linestringCached()).empty(); }
	else if (!isClosed && isRelation) { return !intersectsQuery(layerName, true, multiLinestringCached()).empty(); }
	else if (isRelation){ return !intersectsQuery(layerName, true, multiPolygonCached()).empty(); }
	else                { return !intersectsQuery(layerName, true, polygonCached()).empty(); }
}

vector<string> OsmLuaProcessing::FindCovering(const string &layerName) {
	if      (!isWay   ) { return shpMemTiles.namesOfGeometries(coveredQuery(layerName, false, getPoint())); }
	else if (!isClosed) { return shpMemTiles.namesOfGeometries(coveredQuery(layerName, false, linestringCached())); }
	else if (!isClosed && isRelation) { return shpMemTiles.namesOfGeometries(coveredQuery(layerName, false, multiLinestringCached())); }
	else if (isRelation){ return shpMemTiles.namesOfGeometries(coveredQuery(layerName, false, multiPolygonCached())); }
	else                { return shpMemTiles.namesOfGeometries(coveredQuery(layerName, false, polygonCached())); }
}

bool OsmLuaProcessing::CoveredBy(const string &layerName) {
	if      (!isWay   ) { return !coveredQuery(layerName, true, getPoint()).empty(); }
	else if (!isClosed) { return !coveredQuery(layerName, true, linestringCached()).empty(); }
	else if (!isClosed && isRelation) { return !coveredQuery(layerName, true, multiLinestringCached()).empty(); }
	else if (isRelation){ return !coveredQuery(layerName, true, multiPolygonCached()).empty(); }
	else                { return !coveredQuery(layerName, true, polygonCached()).empty(); }
}

double OsmLuaProcessing::AreaIntersecting(const string &layerName) {
	if      (!isWay || !isClosed) { return 0.0; }
	else if (isRelation){ return intersectsArea(layerName, multiPolygonCached()); }
	else                { return intersectsArea(layerName, polygonCached()); }
}


template <typename GeometryT>
std::vector<uint> OsmLuaProcessing::intersectsQuery(const string &layerName, bool once, GeometryT &geom) const {
	Box box; geom::envelope(geom, box);
	std::vector<uint> ids = shpMemTiles.QueryMatchingGeometries(layerName, once, box,
		[&](const RTree &rtree) { // indexQuery
			vector<IndexValue> results;
			rtree.query(geom::index::intersects(box), back_inserter(results));
			return results;
		},
		[&](OutputObject const &oo) { // checkQuery
			return geom::intersects(geom, osmStore.retrieve_multi_polygon(osmStore.shp(), oo.objectID));
		}
	);
	return ids;
}

template <typename GeometryT>
double OsmLuaProcessing::intersectsArea(const string &layerName, GeometryT &geom) const {
	Box box; geom::envelope(geom, box);
	double area = 0.0;
	std::vector<uint> ids = shpMemTiles.QueryMatchingGeometries(layerName, false, box,
		[&](const RTree &rtree) { // indexQuery
			vector<IndexValue> results;
			rtree.query(geom::index::intersects(box), back_inserter(results));
			return results;
		},
		[&](OutputObject const &oo) { // checkQuery
			MultiPolygon tmp;
			geom::intersection(geom, osmStore.retrieve_multi_polygon(osmStore.shp(), oo.objectID), tmp);
			area += multiPolygonArea(tmp);
			return false;
		}
	);
	return area;
}

template <typename GeometryT>
std::vector<uint> OsmLuaProcessing::coveredQuery(const string &layerName, bool once, GeometryT &geom) const {
	Box box; geom::envelope(geom, box);
	std::vector<uint> ids = shpMemTiles.QueryMatchingGeometries(layerName, once, box,
		[&](const RTree &rtree) { // indexQuery
			vector<IndexValue> results;
			rtree.query(geom::index::intersects(box), back_inserter(results));
			return results;
		},
		[&](OutputObject const &oo) { // checkQuery
			if (oo.geomType!=POLYGON_) return false; // can only be covered by a polygon!
			return geom::covered_by(geom, osmStore.retrieve_multi_polygon(osmStore.shp(), oo.objectID));
		}
	);
	return ids;
}

// Returns whether it is closed polygon
bool OsmLuaProcessing::IsClosed() const {
	if (!isWay) return false; // nonsense: it isn't a way
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
		return multiPolygonArea(multiPolygonCached());
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

double OsmLuaProcessing::multiPolygonArea(const MultiPolygon &mp) const {
	geom::strategy::area::spherical<> sph_strategy(RadiusMeter);
	double totalArea = 0;
	for (MultiPolygon::const_iterator it = mp.begin(); it != mp.end(); ++it) {
		geom::model::polygon<DegPoint> p;
		geom::assign(p,*it);
		geom::for_each_point(p, reverse_project);
		totalArea += geom::area(p, sph_strategy);
	}
	return totalArea;
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
		linestringCache = osmStore.nodeListLinestring(nodeVecPtr->cbegin(),nodeVecPtr->cend());
	}
	return linestringCache;
}

const MultiLinestring &OsmLuaProcessing::multiLinestringCached() {
	if (!multiLinestringInited) {
		multiLinestringInited = true;
		multiLinestringCache = osmStore.wayListMultiLinestring(outerWayVecPtr->cbegin(), outerWayVecPtr->cend());
	}
	return multiLinestringCache;
}

const Polygon &OsmLuaProcessing::polygonCached() {
	if (!polygonInited) {
		polygonInited = true;
		polygonCache = osmStore.nodeListPolygon(nodeVecPtr->cbegin(), nodeVecPtr->cend());
	}
	return polygonCache;
}

const MultiPolygon &OsmLuaProcessing::multiPolygonCached() {
	if (!multiPolygonInited) {
		multiPolygonInited = true;
		multiPolygonCache = osmStore.wayListMultiPolygon(
			outerWayVecPtr->cbegin(), outerWayVecPtr->cend(), innerWayVecPtr->begin(), innerWayVecPtr->cend());

	}
	return multiPolygonCache;
}

// ----	Requests from Lua to write this way/node to a vector tile's Layer

// Add object to specified layer from Lua
void OsmLuaProcessing::Layer(const string &layerName, bool area) {
	if (layers.layerMap.count(layerName) == 0) {
		throw out_of_range("ERROR: Layer(): a layer named as \"" + layerName + "\" doesn't exist.");
	}

	uint layerMinZoom = layers.layers[layers.layerMap[layerName]].minzoom;
	OutputGeometryType geomType = isRelation ? (area ? POLYGON_ : MULTILINESTRING_ ) :
	                                   isWay ? (area ? POLYGON_ : LINESTRING_) : POINT_;
	try {
		if (geomType==POINT_) {
			Point p = Point(lon, latp);

            if(!CorrectGeometry(p)) return;

			refreshOsmID();
			osmStore.store_point(osmStore.osm(), osmID, p);
			OutputObjectRef oo = osmMemTiles.CreateObject(OutputObjectOsmStorePoint(geomType, 
							layers.layerMap[layerName], osmID, attributeStore.empty_set(), layerMinZoom));
			outputs.push_back(std::make_pair(oo, attributeStore.empty_set()));
            return;
		}
		else if (geomType==POLYGON_) {
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

            if(!CorrectGeometry(mp)) return;

			refreshOsmID();
			osmStore.store_multi_polygon(osmStore.osm(), osmID, mp);
			OutputObjectRef oo = osmMemTiles.CreateObject(OutputObjectOsmStoreMultiPolygon(geomType, 
							layers.layerMap[layerName], osmID, attributeStore.empty_set(), layerMinZoom));
			outputs.push_back(std::make_pair(oo, attributeStore.empty_set()));
		}
		else if (geomType==MULTILINESTRING_) {
			// multilinestring
			MultiLinestring mls;
			try {
				mls = multiLinestringCached();
			} catch(std::out_of_range &err) {
				cout << "In relation " << originalOsmID << ": " << err.what() << endl;
				return;
			}
			if (!CorrectGeometry(mls)) return;
			refreshOsmID();
			osmStore.store_multi_linestring(osmStore.osm(), osmID, mls);
			OutputObjectRef oo = osmMemTiles.CreateObject(OutputObjectOsmStoreMultiLinestring(geomType, 
							layers.layerMap[layerName], osmID, attributeStore.empty_set(), layerMinZoom));
			outputs.push_back(std::make_pair(oo, attributeStore.empty_set()));
		}
		else if (geomType==LINESTRING_) {
			// linestring
			Linestring ls = linestringCached();

            if(!CorrectGeometry(ls)) return;

			refreshOsmID();
			osmStore.store_linestring(osmStore.osm(), osmID, ls);
			OutputObjectRef oo = osmMemTiles.CreateObject(OutputObjectOsmStoreLinestring(geomType, 
						layers.layerMap[layerName], osmID, attributeStore.empty_set(), layerMinZoom));
			outputs.push_back(std::make_pair(oo, attributeStore.empty_set()));
		}
	} catch (std::invalid_argument &err) {
		cerr << "Error in OutputObjectOsmStore constructor: " << err.what() << endl;
	}
}

void OsmLuaProcessing::LayerAsCentroid(const string &layerName) {
	if (layers.layerMap.count(layerName) == 0) {
		throw out_of_range("ERROR: LayerAsCentroid(): a layer named as \"" + layerName + "\" doesn't exist.");
	}	

	uint layerMinZoom = layers.layers[layers.layerMap[layerName]].minzoom;
	Point geomp;
	try {
		geomp = calculateCentroid();
		if(geom::is_empty(geomp)) {
			cerr << "Geometry is empty in OsmLuaProcessing::LayerAsCentroid (" << (isRelation ? "relation " : isWay ? "way " : "node ") << originalOsmID << ")" << endl;
			return;
		}

	} catch(std::out_of_range &err) {
		cout << "Couldn't find " << (isRelation ? "relation " : isWay ? "way " : "node " ) << originalOsmID << ": " << err.what() << endl;
		return;
	} catch (geom::centroid_exception &err) {
		cerr << "Problem geometry " << (isRelation ? "relation " : isWay ? "way " : "node " ) << originalOsmID << ": " << err.what() << endl;
		return;
	} catch (std::invalid_argument &err) {
		cerr << "Error in OutputObjectOsmStore constructor for " << (isRelation ? "relation " : isWay ? "way " : "node " ) << originalOsmID << ": " << err.what() << endl;
		return;
	}

	refreshOsmID();
	osmStore.store_point(osmStore.osm(), osmID, geomp);
	OutputObjectRef oo = osmMemTiles.CreateObject(OutputObjectOsmStorePoint(POINT_,
					layers.layerMap[layerName], osmID, attributeStore.empty_set(), layerMinZoom));
	outputs.push_back(std::make_pair(oo, attributeStore.empty_set()));
}

Point OsmLuaProcessing::calculateCentroid() {
	Point centroid;
	if (isRelation) {
		Geometry tmp;
		tmp = osmStore.wayListMultiPolygon(
			outerWayVecPtr->cbegin(), outerWayVecPtr->cend(), innerWayVecPtr->begin(), innerWayVecPtr->cend());
		geom::centroid(tmp, centroid);
		return Point(centroid.x()*10000000.0, centroid.y()*10000000.0);
	} else if (isWay) {
		Polygon p;
		geom::assign_points(p, linestringCached());
		geom::centroid(p, centroid);
		return Point(centroid.x()*10000000.0, centroid.y()*10000000.0);
	} else {
		return Point(lon, latp);
	}
}

std::vector<double> OsmLuaProcessing::Centroid() {
	Point c = calculateCentroid();
	return std::vector<double> { latp2lat(c.y()/10000000.0), c.x()/10000000.0 };
}

// Accept a relation in relation_scan phase
void OsmLuaProcessing::Accept() {
	relationAccepted = true;
}

// If we have more than one layer for a single OSM object,
// we need to generate a new key for the OutputObject
void OsmLuaProcessing::refreshOsmID() {
	if (!osmIDHasBeenUsed) { osmIDHasBeenUsed = true; }
	else if (isWay)        { osmID = spareWayID--; }
	else if (isRelation)   { osmID = spareRelationID--; }
	else                   { osmID = spareNodeID--; }
}

// Set attributes in a vector tile's Attributes table
void OsmLuaProcessing::Attribute(const string &key, const string &val) { AttributeWithMinZoom(key,val,0); }
void OsmLuaProcessing::AttributeWithMinZoom(const string &key, const string &val, const char minzoom) {
	if (val.size()==0) { return; }		// don't set empty strings
	if (outputs.size()==0) { ProcessingError("Can't add Attribute if no Layer set"); return; }
	vector_tile::Tile_Value v;
	v.set_string_value(val);
	outputs.back().second->values.emplace(key, v, minzoom);
	setVectorLayerMetadata(outputs.back().first->layer, key, 0);
}

void OsmLuaProcessing::AttributeNumeric(const string &key, const float val) { AttributeNumericWithMinZoom(key,val,0); }
void OsmLuaProcessing::AttributeNumericWithMinZoom(const string &key, const float val, const char minzoom) {
	if (outputs.size()==0) { ProcessingError("Can't add Attribute if no Layer set"); return; }
	vector_tile::Tile_Value v;
	v.set_float_value(val);
	outputs.back().second->values.emplace(key, v, minzoom);
	setVectorLayerMetadata(outputs.back().first->layer, key, 1);
}

void OsmLuaProcessing::AttributeBoolean(const string &key, const bool val) { AttributeBooleanWithMinZoom(key,val,0); }
void OsmLuaProcessing::AttributeBooleanWithMinZoom(const string &key, const bool val, const char minzoom) {
	if (outputs.size()==0) { ProcessingError("Can't add Attribute if no Layer set"); return; }
	vector_tile::Tile_Value v;
	v.set_bool_value(val);
	outputs.back().second->values.emplace(key, v, minzoom);
	setVectorLayerMetadata(outputs.back().first->layer, key, 2);
}

template<typename T>
static inline T make_valid(double v)
{
	if(!std::isfinite(v)) return 0;
	return static_cast<T>(std::floor(v));
}

// Set minimum zoom
void OsmLuaProcessing::MinZoom(const double z) {
	if (outputs.size()==0) { ProcessingError("Can't set minimum zoom if no Layer set"); return; }
	outputs.back().first->setMinZoom(make_valid<unsigned int>(z));
}

// Set z_order
void OsmLuaProcessing::ZOrder(const double z) {
	if (outputs.size()==0) { ProcessingError("Can't set z_order if no Layer set"); return; }
	outputs.back().first->setZOrder(make_valid<int>(z));
}

// Read scanned relations
kaguya::optional<int> OsmLuaProcessing::NextRelation() {
	relationSubscript++;
	if (relationSubscript >= relationList.size()) return kaguya::nullopt_t();
	return relationList[relationSubscript];
}

std::string OsmLuaProcessing::FindInRelation(const std::string &key) {
	return osmStore.get_relation_tag(relationList[relationSubscript], key);
}

// Record attribute name/type for vector_layers table
void OsmLuaProcessing::setVectorLayerMetadata(const uint_least8_t layer, const string &key, const uint type) {
	layers.layers[layer].attributeMap[key] = type;
}

// Scan relation (but don't write geometry)
// return true if we want it, false if we don't
bool OsmLuaProcessing::scanRelation(WayID id, const tag_map_t &tags) {
	reset();
	originalOsmID = id;
	isWay = false;
	isRelation = true;
	currentTags = tags;
	luaState["relation_scan_function"](this);
	if (!relationAccepted) return false;
	
	osmStore.store_relation_tags(id, tags);
	return true;
}

void OsmLuaProcessing::setNode(NodeID id, LatpLon node, const tag_map_t &tags) {

	reset();
	osmID = (id & OSMID_MASK) | OSMID_NODE;
	osmIDHasBeenUsed = false;
	originalOsmID = id;
	isWay = false;
	isRelation = false;
	lon = node.lon;
	latp= node.latp;
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
void OsmLuaProcessing::setWay(WayID wayId, NodeVec const &nodeVec, const tag_map_t &tags) {
	reset();
	osmID = (wayId & OSMID_MASK) | OSMID_WAY;
	osmIDHasBeenUsed = false;
	originalOsmID = wayId;
	isWay = true;
	isRelation = false;
	nodeVecPtr = &nodeVec;
	outerWayVecPtr = nullptr;
	innerWayVecPtr = nullptr;
	linestringInited = polygonInited = multiPolygonInited = false;

	if (supportsReadingRelations && osmStore.way_in_any_relations(wayId)) {
		relationList = osmStore.relations_for_way(wayId);
	} else {
		relationList.clear();
	}

	try {
		isClosed = nodeVecPtr->front()==nodeVecPtr->back();

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

	if (!this->empty()) {
		for (auto jt = this->outputs.begin(); jt != this->outputs.end(); ++jt) {
			// Store the attributes of the generated geometry
			jt->first->setAttributeSet(attributeStore.store_set(jt->second));		
		}

		// create a list of tiles this way passes through (tileSet)
		unordered_set<TileCoordinates> tileSet;
		try {
			insertIntermediateTiles(osmStore.nodeListLinestring(nodeVecPtr->cbegin(),nodeVecPtr->cend()), this->config.baseZoom, tileSet);

			// then, for each tile, store the OutputObject for each layer
			bool polygonExists = false;
			for (auto it = tileSet.begin(); it != tileSet.end(); ++it) {
				TileCoordinates index = *it;
				for (auto jt = this->outputs.begin(); jt != this->outputs.end(); ++jt) {
					if (jt->first->geomType == POLYGON_) {
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
						if (jt->first->geomType != POLYGON_) continue;
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
void OsmLuaProcessing::setRelation(int64_t relationId, WayVec const &outerWayVec, WayVec const &innerWayVec, const tag_map_t &tags, bool isNativeMP) {
	reset();
	osmID = (relationId & OSMID_MASK) | OSMID_RELATION;
	osmIDHasBeenUsed = false;
	originalOsmID = relationId;
	isWay = true;
	isRelation = true;
	isClosed = isNativeMP;

	nodeVecPtr = nullptr;
	outerWayVecPtr = &outerWayVec;
	innerWayVecPtr = &innerWayVec;
	currentTags = tags;

	bool ok = true;
	if (ok) {
		//Start Lua processing for relation
		luaState[isNativeMP ? "way_function" : "relation_function"](this);
	}
	
	if (this->empty()) return;

	// Assemble multipolygon
	if (isClosed) {
		MultiPolygon mp;
		try {
			// for each tile the relation may cover, put the output objects.
			mp = osmStore.wayListMultiPolygon(outerWayVecPtr->cbegin(), outerWayVecPtr->cend(), innerWayVecPtr->cbegin(), innerWayVecPtr->cend());
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

		for (auto jt = this->outputs.begin(); jt != this->outputs.end(); ++jt) {
			// Store the attributes of the generated geometry
			jt->first->setAttributeSet(attributeStore.store_set(jt->second));		
		}

		for (auto it = tileSet.begin(); it != tileSet.end(); ++it) {
			TileCoordinates index = *it;
			for (auto jt = this->outputs.begin(); jt != this->outputs.end(); ++jt) {
				osmMemTiles.AddObject(index, jt->first);
			}
		}

	// Assemble multilinestring
	} else {
		MultiLinestring mls;
		try {
			mls = osmStore.wayListMultiLinestring(outerWayVecPtr->cbegin(), outerWayVecPtr->cend());
		} catch(std::out_of_range &err) {
			cout << "In relation " << originalOsmID << ": " << err.what() << endl;
			return;
		}

		// Store attributes
		for (auto jt = this->outputs.begin(); jt != this->outputs.end(); ++jt) {
			jt->first->setAttributeSet(attributeStore.store_set(jt->second));		
		}

		// Calculate tileset and then insert outputobject for each one
		for (Linestring ls : mls) {
			unordered_set<TileCoordinates> tileSet;
			insertIntermediateTiles(ls, this->config.baseZoom, tileSet);
			for (auto it = tileSet.begin(); it != tileSet.end(); ++it) {
				TileCoordinates index = *it;
				for (auto jt = this->outputs.begin(); jt != this->outputs.end(); ++jt) {
					osmMemTiles.AddObject(index, jt->first);
				}
			}
		}
	}
}

vector<string> OsmLuaProcessing::GetSignificantNodeKeys() {
	return luaState["node_keys"];
}

