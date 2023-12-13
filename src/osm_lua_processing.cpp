#include <iostream>

#include "osm_lua_processing.h"
#include "attribute_store.h"
#include "helpers.h"
#include "coordinates_geom.h"
#include "osm_mem_tiles.h"


using namespace std;

thread_local kaguya::State *g_luaState = nullptr;
bool supportsRemappingShapefiles = false;
const std::string EMPTY_STRING = "";

int lua_error_handler(int errCode, const char *errMessage)
{
	std::cerr << "lua runtime error: " << std::endl;
	kaguya::util::traceBack(g_luaState->state(), errMessage); // full traceback on 5.2+
	kaguya::util::stackDump(g_luaState->state());
	throw OsmLuaProcessing::luaProcessingException();
}

// ----	initialization routines

OsmLuaProcessing::OsmLuaProcessing(
	OSMStore &osmStore,
	const class Config &configIn,
	class LayerDefinition &layers,
	const string &luaFile,
	const class ShpMemTiles &shpMemTiles, 
	class OsmMemTiles &osmMemTiles,
	AttributeStore &attributeStore,
	bool materializeGeometries):
	osmStore(osmStore),
	shpMemTiles(shpMemTiles),
	osmMemTiles(osmMemTiles),
	attributeStore(attributeStore),
	config(configIn),
	currentTags(NULL),
	layers(layers),
	materializeGeometries(materializeGeometries) {

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
		.addFunction("RestartRelations", &OsmLuaProcessing::RestartRelations)
		.addFunction("FindInRelation", &OsmLuaProcessing::FindInRelation)
	);
	supportsRemappingShapefiles = !!luaState["attribute_function"];
	supportsReadingRelations    = !!luaState["relation_scan_function"];
	supportsWritingRelations    = !!luaState["relation_function"];

	// ---- Call init_function of Lua logic

	if (!!luaState["init_function"]) {
		luaState["init_function"](this->config.projectName);
	}
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
	return currentTags->find(key) != currentTags->end();
}

// Get an OSM tag for a given key (or return empty string if none)
const string& OsmLuaProcessing::Find(const string& key) const {
	auto it = currentTags->find(key);
	if(it == currentTags->end()) return EMPTY_STRING;
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
			return geom::intersects(geom, shpMemTiles.retrieveMultiPolygon(oo.objectID));
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
			geom::intersection(geom, shpMemTiles.retrieveMultiPolygon(oo.objectID), tmp);
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
			return geom::covered_by(geom, shpMemTiles.retrieveMultiPolygon(oo.objectID));
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
		linestringCache = osmStore.llListLinestring(llVecPtr->cbegin(),llVecPtr->cend());
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
		polygonCache = osmStore.llListPolygon(llVecPtr->cbegin(), llVecPtr->cend());
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
	AttributeSet attributes;
	OutputGeometryType geomType = isRelation ? (area ? POLYGON_ : MULTILINESTRING_ ) :
	                                   isWay ? (area ? POLYGON_ : LINESTRING_) : POINT_;
	try {
		// Lua profiles often write the same geometry twice, e.g. a river and its name,
		// a highway and its name. Avoid duplicating geometry processing and storage
		// when this occurs.
		if (lastStoredGeometryId != 0 && lastStoredGeometryType == geomType) {
			OutputObject oo(geomType, layers.layerMap[layerName], lastStoredGeometryId, 0, layerMinZoom);
			outputs.push_back(std::make_pair(std::move(oo), attributes));
			return;
		}

		if (geomType==POINT_) {
			Point p = Point(lon, latp);

			if(CorrectGeometry(p) == CorrectGeometryResult::Invalid) return;

			NodeID id = osmMemTiles.storePoint(p);
			OutputObject oo(geomType, layers.layerMap[layerName], id, 0, layerMinZoom);
			outputs.push_back(std::make_pair(std::move(oo), attributes));
			return;
		}
		else if (geomType==POLYGON_) {
			// polygon

			MultiPolygon mp;

			if (isRelation) {
				try {
					mp = multiPolygonCached();
					if(CorrectGeometry(mp) == CorrectGeometryResult::Invalid) return;
					NodeID id = osmMemTiles.storeMultiPolygon(mp);
					OutputObject oo(geomType, layers.layerMap[layerName], id, 0, layerMinZoom);
					outputs.push_back(std::make_pair(std::move(oo), attributes));
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

				auto correctionResult = CorrectGeometry(mp);
				if(correctionResult == CorrectGeometryResult::Invalid) return;
				NodeID id = 0;
				if (!materializeGeometries && correctionResult == CorrectGeometryResult::Valid) {
					id = USE_WAY_STORE | originalOsmID;
					wayEmitted = true;
				} else 
					id = osmMemTiles.storeMultiPolygon(mp);
				OutputObject oo(geomType, layers.layerMap[layerName], id, 0, layerMinZoom);
				outputs.push_back(std::make_pair(std::move(oo), attributes));
			}

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
			if (CorrectGeometry(mls) == CorrectGeometryResult::Invalid) return;

			NodeID id = osmMemTiles.storeMultiLinestring(mls);
			lastStoredGeometryId = id;
			lastStoredGeometryType = geomType;
			OutputObject oo(geomType, layers.layerMap[layerName], id, 0, layerMinZoom);
			outputs.push_back(std::make_pair(std::move(oo), attributes));
		}
		else if (geomType==LINESTRING_) {
			// linestring
			Linestring ls = linestringCached();

			auto correctionResult = CorrectGeometry(ls);
			if(correctionResult == CorrectGeometryResult::Invalid) return;

			if (isWay && !isRelation) {
				NodeID id = 0;
				if (!materializeGeometries && correctionResult == CorrectGeometryResult::Valid) {
					id = USE_WAY_STORE | originalOsmID;
					wayEmitted = true;
				}	else 
					id = osmMemTiles.storeLinestring(ls);
				lastStoredGeometryId = id;
				lastStoredGeometryType = geomType;
				OutputObject oo(geomType, layers.layerMap[layerName], id, 0, layerMinZoom);
				outputs.push_back(std::make_pair(std::move(oo), attributes));
			} else {
				NodeID id = osmMemTiles.storeLinestring(ls);
				lastStoredGeometryId = id;
				lastStoredGeometryType = geomType;
				OutputObject oo(geomType, layers.layerMap[layerName], id, 0, layerMinZoom);
				outputs.push_back(std::make_pair(std::move(oo), attributes));
			}
		}
	} catch (std::invalid_argument &err) {
		cerr << "Error in OutputObject constructor: " << err.what() << endl;
	}
}

void OsmLuaProcessing::LayerAsCentroid(const string &layerName) {
	if (layers.layerMap.count(layerName) == 0) {
		throw out_of_range("ERROR: LayerAsCentroid(): a layer named as \"" + layerName + "\" doesn't exist.");
	}	

	uint layerMinZoom = layers.layers[layers.layerMap[layerName]].minzoom;
	AttributeSet attributes;
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
		if (verbose) cerr << "Problem geometry " << (isRelation ? "relation " : isWay ? "way " : "node " ) << originalOsmID << ": " << err.what() << endl;
		return;
	} catch (std::invalid_argument &err) {
		cerr << "Error in OutputObject constructor for " << (isRelation ? "relation " : isWay ? "way " : "node " ) << originalOsmID << ": " << err.what() << endl;
		return;
	}

	NodeID id = osmMemTiles.storePoint(geomp);
	OutputObject oo(POINT_, layers.layerMap[layerName], id, 0, layerMinZoom);
	outputs.push_back(std::make_pair(std::move(oo), attributes));
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

// Set attributes in a vector tile's Attributes table
void OsmLuaProcessing::Attribute(const string &key, const string &val) { AttributeWithMinZoom(key,val,0); }
void OsmLuaProcessing::AttributeWithMinZoom(const string &key, const string &val, const char minzoom) {
	if (val.size()==0) { return; }		// don't set empty strings
	if (outputs.size()==0) { ProcessingError("Can't add Attribute if no Layer set"); return; }
	attributeStore.addAttribute(outputs.back().second, key, val, minzoom);
	setVectorLayerMetadata(outputs.back().first.layer, key, 0);
}

void OsmLuaProcessing::AttributeNumeric(const string &key, const float val) { AttributeNumericWithMinZoom(key,val,0); }
void OsmLuaProcessing::AttributeNumericWithMinZoom(const string &key, const float val, const char minzoom) {
	if (outputs.size()==0) { ProcessingError("Can't add Attribute if no Layer set"); return; }
	attributeStore.addAttribute(outputs.back().second, key, val, minzoom);
	setVectorLayerMetadata(outputs.back().first.layer, key, 1);
}

void OsmLuaProcessing::AttributeBoolean(const string &key, const bool val) { AttributeBooleanWithMinZoom(key,val,0); }
void OsmLuaProcessing::AttributeBooleanWithMinZoom(const string &key, const bool val, const char minzoom) {
	if (outputs.size()==0) { ProcessingError("Can't add Attribute if no Layer set"); return; }
	attributeStore.addAttribute(outputs.back().second, key, val, minzoom);
	setVectorLayerMetadata(outputs.back().first.layer, key, 2);
}

// Set minimum zoom
void OsmLuaProcessing::MinZoom(const double z) {
	if (outputs.size()==0) { ProcessingError("Can't set minimum zoom if no Layer set"); return; }
	outputs.back().first.setMinZoom(z);
}

// Set z_order
void OsmLuaProcessing::ZOrder(const double z) {
	if (outputs.size()==0) { ProcessingError("Can't set z_order if no Layer set"); return; }
	outputs.back().first.setZOrder(z);
}

// Read scanned relations
kaguya::optional<int> OsmLuaProcessing::NextRelation() {
	relationSubscript++;
	if (relationSubscript >= relationList.size()) return kaguya::nullopt_t();
	return relationList[relationSubscript];
}

void OsmLuaProcessing::RestartRelations() {
	relationSubscript = -1;
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
	currentTags = &tags;
	try {
		luaState["relation_scan_function"](this);
	} catch(luaProcessingException &e) {
		std::cerr << "Lua error on scanning relation " << originalOsmID << std::endl;
		exit(1);
	}
	if (!relationAccepted) return false;
	
	osmStore.store_relation_tags(id, tags);
	return true;
}

void OsmLuaProcessing::setNode(NodeID id, LatpLon node, const tag_map_t &tags) {

	reset();
	originalOsmID = id;
	isWay = false;
	isRelation = false;
	lon = node.lon;
	latp= node.latp;
	currentTags = &tags;

	//Start Lua processing for node
	try {
		luaState["node_function"](this);
	} catch(luaProcessingException &e) {
		std::cerr << "Lua error on node " << originalOsmID << std::endl;
		exit(1);
	}

	if (!this->empty()) {
		TileCoordinates index = latpLon2index(node, this->config.baseZoom);

		for (auto &output : finalizeOutputs()) {
			osmMemTiles.addObjectToSmallIndex(index, output, originalOsmID);
		}
	} 
}

// We are now processing a way
bool OsmLuaProcessing::setWay(WayID wayId, LatpLonVec const &llVec, const tag_map_t &tags) {
	reset();
	wayEmitted = false;
	originalOsmID = wayId;
	isWay = true;
	isRelation = false;
	llVecPtr = &llVec;
	outerWayVecPtr = nullptr;
	innerWayVecPtr = nullptr;
	linestringInited = polygonInited = multiPolygonInited = false;

	if (supportsReadingRelations && osmStore.way_in_any_relations(wayId)) {
		relationList = osmStore.relations_for_way(wayId);
	} else {
		relationList.clear();
	}

	try {
		isClosed = llVecPtr->front()==llVecPtr->back();

	} catch (std::out_of_range &err) {
		std::stringstream ss;
		ss << "Way " << originalOsmID << " is missing a node";
		throw std::out_of_range(ss.str());
	}

	currentTags = &tags;

	bool ok = true;
	if (ok) {
		//Start Lua processing for way
		try {
			kaguya::LuaFunction way_function = luaState["way_function"];
			kaguya::LuaRef ret = way_function(this);
			assert(!ret);
		} catch(luaProcessingException &e) {
			std::cerr << "Lua error on way " << originalOsmID << std::endl;
			exit(1);
		}
	}

	if (!this->empty()) {
		osmMemTiles.addGeometryToIndex(linestringCached(), finalizeOutputs(), originalOsmID);
		return wayEmitted;
	}

	return false;
}

// We are now processing a relation
void OsmLuaProcessing::setRelation(int64_t relationId, WayVec const &outerWayVec, WayVec const &innerWayVec, const tag_map_t &tags, 
                                   bool isNativeMP,      // only OSM type=multipolygon
                                   bool isInnerOuter) {  // any OSM relation with "inner" and "outer" roles (e.g. type=multipolygon|boundary)
	reset();
	originalOsmID = relationId;
	isWay = true;
	isRelation = true;
	isClosed = isNativeMP || isInnerOuter;

	llVecPtr = nullptr;
	outerWayVecPtr = &outerWayVec;
	innerWayVecPtr = &innerWayVec;
	currentTags = &tags;

	// Start Lua processing for relation
	if (!isNativeMP && !supportsWritingRelations) return;
	try {
		luaState[isNativeMP ? "way_function" : "relation_function"](this);
	} catch(luaProcessingException &e) {
		std::cerr << "Lua error on relation " << originalOsmID << std::endl;
		exit(1);
	}
	if (this->empty()) return;

	try {
		if (isClosed) {
			std::vector<OutputObject> objects = finalizeOutputs();
			osmMemTiles.addGeometryToIndex(multiPolygonCached(), objects, originalOsmID);
		} else {
			osmMemTiles.addGeometryToIndex(multiLinestringCached(), finalizeOutputs(), originalOsmID);
		}
	} catch(std::out_of_range &err) {
		cout << "In relation " << originalOsmID << ": " << err.what() << endl;
	}		
}

vector<string> OsmLuaProcessing::GetSignificantNodeKeys() {
	return luaState["node_keys"];
}

std::vector<OutputObject> OsmLuaProcessing::finalizeOutputs() {
	std::vector<OutputObject> list;
	list.reserve(this->outputs.size());
	for (auto jt = this->outputs.begin(); jt != this->outputs.end(); ++jt) {
		jt->first.setAttributeSet(attributeStore.add(jt->second));
		list.push_back(jt->first);
	}
	return list;
}

