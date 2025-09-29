#include <iostream>

#include "osm_lua_processing.h"
#include "attribute_store.h"
#include "helpers.h"
#include "coordinates_geom.h"
#include "osm_mem_tiles.h"
#include "significant_tags.h"
#include "tag_map.h"
#include "node_store.h"
#include "polylabel.h"
#include <signal.h>

using namespace std;

const std::string EMPTY_STRING = "";
thread_local kaguya::State *g_luaState = nullptr;
thread_local OsmLuaProcessing* osmLuaProcessing = nullptr;

std::mutex vectorLayerMetadataMutex;
std::unordered_map<std::string, std::string> OsmLuaProcessing::dataStore;
std::mutex OsmLuaProcessing::dataStoreMutex;

void handleOsmLuaProcessingUserSignal(int signum) {
	osmLuaProcessing->handleUserSignal(signum);
}

class Sigusr1Handler {
public:
	Sigusr1Handler() {
#ifndef _WIN32
		signal(SIGUSR1, handleOsmLuaProcessingUserSignal);
#endif
	}

	void initialize() {
		// No-op just to ensure the compiler doesn't optimize away
		// the handler.
	}
};

thread_local Sigusr1Handler sigusr1Handler;

// A key in `currentTags`. If Lua code refers to an absent key,
// found will be false.
struct KnownTagKey {
	bool found;
	uint32_t index;

	// stringValue is populated only in PostScanRelations phase; we could consider
	// having osm_store's relationTags use TagMap, in which case we'd be able to
	// use the found/index fields
	std::string stringValue;
};

template<>  struct kaguya::lua_type_traits<KnownTagKey> {
	typedef KnownTagKey get_type;
	typedef const KnownTagKey& push_type;

	static bool strictCheckType(lua_State* l, int index)
	{
		return lua_type(l, index) == LUA_TSTRING;
	}
	static bool checkType(lua_State* l, int index)
	{
		return lua_isstring(l, index) != 0;
	}
	static get_type get(lua_State* l, int index)
	{
		KnownTagKey rv = { false, 0 };
		size_t size = 0;
		const char* buffer = lua_tolstring(l, index, &size);

		if (osmLuaProcessing->isPostScanRelation) {
			// In this phase, the Holds/Find functions directly query a
			// traditional string->string map, so just ensure we expose
			// the string.
			rv.stringValue = std::string(buffer, size);
			return rv;
		}


		int64_t tagLoc = osmLuaProcessing->currentTags->getKey(buffer, size);

		if (tagLoc >= 0) {
			rv.found = true;
			rv.index = tagLoc;
		}
//		std::string key(buffer, size);
//		std::cout << "for key " << key << ": rv.found=" << rv.found << ", rv.index=" << rv.index << std::endl;
		return rv;
	}
	static int push(lua_State* l, push_type s)
	{
		throw std::runtime_error("Lua code doesn't know how to use KnownTagKey");
	}
};

template<>  struct kaguya::lua_type_traits<protozero::data_view> {
	typedef protozero::data_view get_type;
	typedef const protozero::data_view& push_type;

	static bool strictCheckType(lua_State* l, int index)
	{
		return lua_type(l, index) == LUA_TSTRING;
	}
	static bool checkType(lua_State* l, int index)
	{
		return lua_isstring(l, index) != 0;
	}
	static get_type get(lua_State* l, int index)
	{
		size_t size = 0;
		const char* buffer = lua_tolstring(l, index, &size);
		protozero::data_view rv = { buffer, size };
		return rv;
	}
	static int push(lua_State* l, push_type s)
	{
		throw std::runtime_error("Lua code doesn't know how to use protozero::data_view");
	}
};

// Gets a table of all the keys of the OSM tags
kaguya::LuaTable getAllKeys(kaguya::State& luaState, const boost::container::flat_map<std::string, std::string>* tags) {
	kaguya::LuaTable tagsTable = luaState.newTable();
	int index = 1; // Lua is 1-based
	for (const auto& kv: *tags) {
		tagsTable[index++] = kv.first;
	}
	return tagsTable;
}

// Gets a table of all the OSM tags
kaguya::LuaTable getAllTags(kaguya::State& luaState, const boost::container::flat_map<std::string, std::string>* tags) {
	kaguya::LuaTable tagsTable = luaState.newTable();
	for (const auto& kv: *tags) {
		tagsTable[kv.first] = kv.second;
	}
	return tagsTable;
}

std::string rawId() { return osmLuaProcessing->Id(); }
std::string rawOsmType() { return osmLuaProcessing->OsmType(); }
kaguya::LuaTable rawAllKeys() {
	if (osmLuaProcessing->isPostScanRelation) {
		return osmLuaProcessing->AllKeys(*g_luaState);
	}

	auto tags = osmLuaProcessing->currentTags->exportToBoostMap();

	return getAllKeys(*g_luaState, &tags);
}kaguya::LuaTable rawAllTags() {
	if (osmLuaProcessing->isPostScanRelation) {
		return osmLuaProcessing->AllTags(*g_luaState);
	}

	auto tags = osmLuaProcessing->currentTags->exportToBoostMap();

	return getAllTags(*g_luaState, &tags);
}
bool rawHolds(const KnownTagKey& key) {
	if (osmLuaProcessing->isPostScanRelation) {
		return osmLuaProcessing->Holds(key.stringValue);
	}

	return key.found;
}
bool rawHasTags() { return osmLuaProcessing->HasTags(); }
void rawSetTag(const std::string &key, const std::string &value) { return osmLuaProcessing->SetTag(key, value); }
const std::string rawFind(const KnownTagKey& key) {
	if (osmLuaProcessing->isPostScanRelation)
		return osmLuaProcessing->Find(key.stringValue);

	if (key.found) {
		auto value = *(osmLuaProcessing->currentTags->getValueFromKey(key.index));
		return std::string(value.data(), value.size());
	}

	return EMPTY_STRING;
}
std::vector<std::string> rawFindIntersecting(const std::string &layerName) { return osmLuaProcessing->FindIntersecting(layerName); }
bool rawIntersects(const std::string& layerName) { return osmLuaProcessing->Intersects(layerName); }
std::vector<std::string> rawFindCovering(const std::string& layerName) { return osmLuaProcessing->FindCovering(layerName); }
bool rawCoveredBy(const std::string& layerName) { return osmLuaProcessing->CoveredBy(layerName); }
bool rawIsClosed() { return osmLuaProcessing->IsClosed(); }
bool rawIsMultiPolygon() { return osmLuaProcessing->IsMultiPolygon(); }
double rawArea() { return osmLuaProcessing->Area(); }
double rawLength() { return osmLuaProcessing->Length(); }
kaguya::optional<std::vector<double>> rawCentroid(kaguya::VariadicArgType algorithm) { return osmLuaProcessing->Centroid(algorithm); }
void rawLayer(const std::string& layerName, bool area) { return osmLuaProcessing->Layer(layerName, area); }
void rawLayerAsCentroid(const std::string &layerName, kaguya::VariadicArgType nodeSources) { return osmLuaProcessing->LayerAsCentroid(layerName, nodeSources); }
void rawMinZoom(const double z) { return osmLuaProcessing->MinZoom(z); }
void rawZOrder(const double z) { return osmLuaProcessing->ZOrder(z); }
OsmLuaProcessing::OptionalRelation rawNextRelation() { return osmLuaProcessing->NextRelation(); }
void rawRestartRelations() { return osmLuaProcessing->RestartRelations(); }
std::string rawFindInRelation(const std::string& key) { return osmLuaProcessing->FindInRelation(key); }
void rawAccept() { return osmLuaProcessing->Accept(); }
double rawAreaIntersecting(const std::string& layerName) { return osmLuaProcessing->AreaIntersecting(layerName); }

void rawSetData(const std::string &key, const std::string &value) { 
	std::lock_guard<std::mutex> lock(osmLuaProcessing->dataStoreMutex);
	osmLuaProcessing->dataStore[key] = value;
}
std::string rawGetData(const std::string &key) {
	auto r = osmLuaProcessing->dataStore.find(key);
	return r==osmLuaProcessing->dataStore.end() ? "" : r->second;
}

bool supportsRemappingShapefiles = false;

int lua_error_handler(int errCode, const char *errMessage)
{
	std::cerr << "lua runtime error " << std::to_string(errCode) << ":" << std::endl;
	std::cerr << errMessage << std::endl;
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
	bool materializeGeometries,
	bool isFirst) :
	osmStore(osmStore),
	shpMemTiles(shpMemTiles),
	osmMemTiles(osmMemTiles),
	attributeStore(attributeStore),
	config(configIn),
	currentTags(NULL),
	layers(layers),
	materializeGeometries(materializeGeometries) {

	sigusr1Handler.initialize();

	// ----	Initialise Lua
	g_luaState = &luaState;
	luaState.setErrorHandler(lua_error_handler);
	luaState.dofile(luaFile.c_str());

	osmLuaProcessing = this;
	luaState["Id"] = &rawId;
	luaState["OsmType"] = &rawOsmType;
	luaState["AllKeys"] = &rawAllKeys;
	luaState["AllTags"] = &rawAllTags;
	luaState["Holds"] = &rawHolds;
	luaState["Find"] = &rawFind;
	luaState["HasTags"] = &rawHasTags;
	luaState["SetTag"] = &rawSetTag;
	luaState["FindIntersecting"] = &rawFindIntersecting;
	luaState["Intersects"] = &rawIntersects;
	luaState["FindCovering"] = &rawFindCovering;
	luaState["CoveredBy"] = &rawCoveredBy;
	luaState["IsClosed"] = &rawIsClosed;
	luaState["IsMultiPolygon"] = &rawIsMultiPolygon;
	luaState["Area"] = &rawArea;
	luaState["AreaIntersecting"] = &rawAreaIntersecting;
	luaState["Length"] = &rawLength;
	luaState["Centroid"] = &rawCentroid;
	luaState["Layer"] = &rawLayer;
	luaState["LayerAsCentroid"] = &rawLayerAsCentroid;
	luaState["Attribute"] = kaguya::overload(
			[](const std::string &key, const protozero::data_view val) { osmLuaProcessing->Attribute(key, val, 0); },
			[](const std::string &key, const protozero::data_view val, const char minzoom) { osmLuaProcessing->Attribute(key, val, minzoom); }
	);
	luaState["AttributeNumeric"] = kaguya::overload(
			[](const std::string &key, const double val) { osmLuaProcessing->AttributeNumeric(key, val, 0); },
			[](const std::string &key, const double val, const char minzoom) { osmLuaProcessing->AttributeNumeric(key, val, minzoom); }
	);
	luaState["AttributeInteger"] = kaguya::overload(
			[](const std::string &key, const int val) { osmLuaProcessing->AttributeInteger(key, val, 0); },
			[](const std::string &key, const int val, const char minzoom) { osmLuaProcessing->AttributeInteger(key, val, minzoom); }
	);
	luaState["AttributeBoolean"] = kaguya::overload(
			[](const std::string &key, const bool val) { osmLuaProcessing->AttributeBoolean(key, val, 0); },
			[](const std::string &key, const bool val, const char minzoom) { osmLuaProcessing->AttributeBoolean(key, val, minzoom); }
	);

	luaState["MinZoom"] = &rawMinZoom;
	luaState["ZOrder"] = &rawZOrder;
	luaState["Accept"] = &rawAccept;
	luaState["NextRelation"] = &rawNextRelation;
	luaState["RestartRelations"] = &rawRestartRelations;
	luaState["FindInRelation"] = &rawFindInRelation;
	luaState["SetData"] = &rawSetData;
	luaState["GetData"] = &rawGetData;
	supportsRemappingShapefiles = !!luaState["attribute_function"];
	supportsReadingRelations    = !!luaState["relation_scan_function"];
	supportsPostScanRelations   = !!luaState["relation_postscan_function"];
	supportsWritingNodes        = !!luaState["node_function"];
	supportsWritingWays         = !!luaState["way_function"];
	supportsWritingRelations    = !!luaState["relation_function"];

	// ---- Call init_function of Lua logic

	if (!!luaState["init_function"]) {
		luaState["init_function"](this->config.projectName, isFirst);
	}
}

OsmLuaProcessing::~OsmLuaProcessing() {
	// Call exit_function of Lua logic
	luaState("if exit_function~=nil then exit_function() end");
}

void OsmLuaProcessing::handleUserSignal(int signum) {
	std::cout << "processing OSM ID " << originalOsmID << std::endl;
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

bool OsmLuaProcessing::canPostScanRelations() {
	return supportsPostScanRelations;
}

bool OsmLuaProcessing::canWriteNodes() {
	return supportsWritingNodes;
}

bool OsmLuaProcessing::canWriteWays() {
	return supportsWritingWays;
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

// Get the Type of the current object
string OsmLuaProcessing::OsmType() const {
	return (isRelation ? "relation" : isWay ? "way" : "node");
}

// Gets a table of all the keys of the OSM tags
kaguya::LuaTable OsmLuaProcessing::AllKeys(kaguya::State& luaState) {
	// NOTE: this is only called in the PostScanRelation phase -- other phases are handled in rawAllKeys
	return getAllKeys(luaState, currentPostScanTags);
}

// Gets a table of all the OSM tags
kaguya::LuaTable OsmLuaProcessing::AllTags(kaguya::State& luaState) {
	// NOTE: this is only called in the PostScanRelation phase -- other phases are handled in rawAllTags
	return getAllTags(luaState, currentPostScanTags);
}

// Check if there's a value for a given key
bool OsmLuaProcessing::Holds(const string& key) const {
	// NOTE: this is only called in the PostScanRelation phase -- other phases are handled in rawHolds
	return currentPostScanTags->find(key)!=currentPostScanTags->end();
}

// Get an OSM tag for a given key (or return empty string if none)
const string OsmLuaProcessing::Find(const string& key) const {
	// NOTE: this is only called in the PostScanRelation phase -- other phases are handled in rawFind
	auto it = currentPostScanTags->find(key);
	if (it == currentPostScanTags->end()) return EMPTY_STRING;
	return it->second;
}

// Check if an object has any tags
bool OsmLuaProcessing::HasTags() const {
	return isPostScanRelation ? !currentPostScanTags->empty() : !currentTags->empty();
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
	if (!shpMemTiles.mayIntersect(layerName, box))
		return std::vector<uint>();

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

// Return whether it's a multipolygon
bool OsmLuaProcessing::IsMultiPolygon() const {
	return isWay && isRelation;
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
	if (isWay && !isRelation) {
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
	outputKeys.clear();
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

			NodeID id = USE_NODE_STORE | originalOsmID;
			if (materializeGeometries)
				id = osmMemTiles.storePoint(p);
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

// LayerAsCentroid(layerName, [centroid-algorithm, [role, [role, ...]]])
//
// Emit a point. This function can be called for nodes, ways or relations.
//
// When called for a 2D geometry, you can pass a preferred centroid algorithm
// in `centroid-algorithm`. Currently `polylabel` and `centroid` are supported.
//
// When called for a relation, you can pass a list of roles. The point of a node
// with that role will be used if available.
void OsmLuaProcessing::LayerAsCentroid(const string &layerName, kaguya::VariadicArgType varargs) {
	outputKeys.clear();
	if (layers.layerMap.count(layerName) == 0) {
		throw out_of_range("ERROR: LayerAsCentroid(): a layer named as \"" + layerName + "\" doesn't exist.");
	}	

	CentroidAlgorithm algorithm = defaultCentroidAlgorithm();

	for (auto needleRef : varargs) {
		const std::string needle = needleRef.get<std::string>();
		algorithm = parseCentroidAlgorithm(needle);
		break;
	}

	// This will be non-zero if we ultimately used a node from a relation to
	// label the point.
	NodeID relationNode = 0;

	uint layerMinZoom = layers.layers[layers.layerMap[layerName]].minzoom;
	AttributeSet attributes;
	Point geomp;
	bool centroidFound = false;
	try {
		// If we're a relation, see if the user would prefer we use one of its members
		// to label the point.
		if (isRelation) {
			int i = -1;
			for (auto needleRef : varargs) {
				i++;
				// Skip the first vararg, it's the algorithm.
				if (i == 0) continue;
				const std::string needle = needleRef.get<std::string>();

				// We do a linear search of the relation's members. This is not very efficient
				// for relations like Tongass National Park (ID 6535292, 29,000 members),
				// but in general, it's probably fine.
				//
				// I note that relation members seem to be sorted nodes first, then ways,
				// then relations. I'm not sure if we can rely on that, so I don't
				// short-circuit on the first non-node.
				for (int i = 0; i < currentRelation->memids.size(); i++) {
					if (currentRelation->types[i] != PbfReader::Relation::MemberType::NODE)
						continue;

					const protozero::data_view role = stringTable->at(currentRelation->roles_sid[i]);
					if (role.size() == needle.size() && 0 == memcmp(role.data(), needle.data(), role.size())) {
						relationNode = currentRelation->memids[i];
						const auto ll = osmStore.nodes.at(relationNode);
						geomp = Point(ll.lon, ll.latp);
						centroidFound = true;
						break;
					}
				}

				if (relationNode != 0)
					break;
			}
		}

		if (!centroidFound)
			geomp = calculateCentroid(algorithm);

		// TODO: I think geom::is_empty always returns false for Points?
		// See https://github.com/boostorg/geometry/blob/fa3623528ea27ba2c3c1327e4b67408a2b567038/include/boost/geometry/algorithms/is_empty.hpp#L103
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

	NodeID id = 0;
	// We don't do lazy geometries for centroids in these cases:
	//
	// - --materialize-geometries is set
	// - the geometry is a relation - calculating their centroid can be quite expensive,
	//   and there's not as many of them as there are ways
	// - when the algorithm chosen is polylabel
	//     We can extend lazy geometries to this, it just needs some fiddling to
	//     express it in the ID and measure if there's a runtime impact in computing
	//     the polylabel twice.
	if (materializeGeometries || (isRelation && relationNode == 0) || (isWay && algorithm != CentroidAlgorithm::Centroid)) {
		id = osmMemTiles.storePoint(geomp);
	} else if (relationNode != 0) {
		id = USE_NODE_STORE | relationNode;
	} else if (!isRelation && !isWay) {
		// Sometimes people call LayerAsCentroid(...) on a node, because they're
		// writing a generic handler that doesn't know if it's a node or a way,
		// e.g. POIs.
		id = USE_NODE_STORE | originalOsmID;
	} else {
		id = USE_WAY_STORE | originalOsmID;
		wayEmitted = true;
	}
	OutputObject oo(POINT_, layers.layerMap[layerName], id, 0, layerMinZoom);
	outputs.push_back(std::make_pair(std::move(oo), attributes));
}

Point OsmLuaProcessing::calculateCentroid(CentroidAlgorithm algorithm) {
	Point centroid;
	if (isRelation) {
		MultiPolygon tmp;
		tmp = multiPolygonCached();

		if (algorithm == CentroidAlgorithm::Polylabel) {
			int index = 0;

			// CONSIDER: pick precision intelligently
			// Polylabel works on polygons, so for multipolygons we'll label the biggest outer.
			double biggestSize = 0;
			for (int i = 0; i < tmp.size(); i++) {
				double size = geom::area(tmp[i]);
				if (size > biggestSize) {
					biggestSize = size;
					index = i;
				}
			}

			if (tmp.size() == 0)
				throw geom::centroid_exception();
			centroid = mapbox::polylabel(tmp[index]);
		} else {
			geom::centroid(tmp, centroid);
		}
		return Point(centroid.x()*10000000.0, centroid.y()*10000000.0);
	} else if (isWay) {
		Polygon p;
		geom::assign_points(p, linestringCached());

		if (algorithm == CentroidAlgorithm::Polylabel) {
			// CONSIDER: pick precision intelligently
			centroid = mapbox::polylabel(p);
		} else {
			geom::centroid(p, centroid);
		}
		return Point(centroid.x()*10000000.0, centroid.y()*10000000.0);
	} else {
		return Point(lon, latp);
	}
}

OsmLuaProcessing::CentroidAlgorithm OsmLuaProcessing::parseCentroidAlgorithm(const std::string& algorithm) const {
	if (algorithm == "polylabel") return OsmLuaProcessing::CentroidAlgorithm::Polylabel;
	if (algorithm == "centroid") return OsmLuaProcessing::CentroidAlgorithm::Centroid;

	throw std::runtime_error("unknown centroid algorithm " + algorithm);
}

kaguya::optional<std::vector<double>> OsmLuaProcessing::Centroid(kaguya::VariadicArgType algorithmArgs) {
	CentroidAlgorithm algorithm = defaultCentroidAlgorithm();

	for (auto needleRef : algorithmArgs) {
		const std::string needle = needleRef.get<std::string>();
		algorithm = parseCentroidAlgorithm(needle);
		break;
	}
	try {
		Point c = calculateCentroid(algorithm);
		return std::vector<double> { latp2lat(c.y()/10000000.0), c.x()/10000000.0 };
	} catch (geom::centroid_exception &err) {
		if (verbose) cerr << "Problem geometry " << (isRelation ? "relation " : isWay ? "way " : "node " ) << originalOsmID << ": " << err.what() << endl;
		return kaguya::optional<std::vector<double>>();
	}
}

// Accept a relation in relation_scan phase
void OsmLuaProcessing::Accept() {
	relationAccepted = true;
}
// Set a tag in post-scan phase
void OsmLuaProcessing::SetTag(const std::string &key, const std::string &value) {
	if (!isPostScanRelation) throw std::runtime_error("SetTag can only be used in relation_postscan_function");
	osmStore.scannedRelations.set_relation_tag(originalOsmID, key, value);
}

void OsmLuaProcessing::removeAttributeIfNeeded(const string& key) {
	// Does it exist?
	for (int i = 0; i < outputKeys.size(); i++) {
		if (outputKeys[i] == key) {
			AttributeSet& set = outputs.back().second;
			set.removePairWithKey(attributeStore.pairStore, attributeStore.keyStore.key2index(key));
			return;
		}
	}

	outputKeys.push_back(key);
}

// Set attributes in a vector tile's Attributes table
void OsmLuaProcessing::Attribute(const string &key, const protozero::data_view val, const char minzoom) {
	if (outputs.size()==0) { ProcessingError("Can't add Attribute if no Layer set"); return; }
	removeAttributeIfNeeded(key);
	attributeStore.addAttribute(outputs.back().second, key, val, minzoom);
	setVectorLayerMetadata(outputs.back().first.layer, key, 0);
}

void OsmLuaProcessing::AttributeNumeric(const string &key, const double val, const char minzoom) {
	if (outputs.size()==0) { ProcessingError("Can't add Attribute if no Layer set"); return; }
	removeAttributeIfNeeded(key);
	attributeStore.addAttribute(outputs.back().second, key, val, minzoom);
	setVectorLayerMetadata(outputs.back().first.layer, key, 1);
}

void OsmLuaProcessing::AttributeBoolean(const string &key, const bool val, const char minzoom) {
	if (outputs.size()==0) { ProcessingError("Can't add Attribute if no Layer set"); return; }
	removeAttributeIfNeeded(key);
	attributeStore.addAttribute(outputs.back().second, key, val, minzoom);
	setVectorLayerMetadata(outputs.back().first.layer, key, 2);
}

void OsmLuaProcessing::AttributeInteger(const string &key, const int val, const char minzoom) {
	if (outputs.size()==0) { ProcessingError("Can't add Attribute if no Layer set"); return; }
	removeAttributeIfNeeded(key);
	attributeStore.addAttribute(outputs.back().second, key, val, minzoom);
	setVectorLayerMetadata(outputs.back().first.layer, key, 3);
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
// Kaguya doesn't support optional<tuple<int,string>>, so we write a custom serializer
// to either return nil or a tuple.
template<>  struct kaguya::lua_type_traits<OsmLuaProcessing::OptionalRelation> {
	typedef OsmLuaProcessing::OptionalRelation get_type;
	typedef const OsmLuaProcessing::OptionalRelation& push_type;

	static bool strictCheckType(lua_State* l, int index)
	{
		throw std::runtime_error("Lua code doesn't know how to send OptionalRelation");
	}
	static bool checkType(lua_State* l, int index)
	{
		throw std::runtime_error("Lua code doesn't know how to send OptionalRelation");
	}
	static get_type get(lua_State* l, int index)
	{
		throw std::runtime_error("Lua code doesn't know how to send OptionalRelation");
	}
	static int push(lua_State* l, push_type s)
	{
		if (s.done)
			return 0;

		lua_pushinteger(l, s.id);
		lua_pushlstring(l, s.role.data(), s.role.size());
		return 2;
	}
};

OsmLuaProcessing::OptionalRelation OsmLuaProcessing::NextRelation() {
	relationSubscript++;
	if (relationSubscript >= relationList.size()) return { true };

	return {
		false,
		static_cast<lua_Integer>(relationList[relationSubscript].first),
		osmStore.scannedRelations.getRole(relationList[relationSubscript].second)
	};
}

void OsmLuaProcessing::RestartRelations() {
	relationSubscript = -1;
}

std::string OsmLuaProcessing::FindInRelation(const std::string &key) {
	return osmStore.scannedRelations.get_relation_tag(relationList[relationSubscript].first, key);
}

// Record attribute name/type for vector_layers table
void OsmLuaProcessing::setVectorLayerMetadata(const uint_least8_t layer, const string &key, const uint type) {
	std::lock_guard<std::mutex> lock(vectorLayerMetadataMutex);
	layers.layers[layer].attributeMap[key] = type;
}

// Scan relation (but don't write geometry)
// return true if we want it, false if we don't
bool OsmLuaProcessing::scanRelation(WayID id, const TagMap& tags) {
	reset();
	originalOsmID = id;
	isRelation = true;
	currentTags = &tags;
	try {
		luaState["relation_scan_function"]();
	} catch(luaProcessingException &e) {
		std::cerr << "Lua error on scanning relation " << originalOsmID << std::endl;
		exit(1);
	}
	if (!relationAccepted) return false;
	
	// If we're persisting, we need to make a real map that owns its
	// own keys and values.
	osmStore.scannedRelations.store_relation_tags(id, tags.exportToBoostMap());
	return true;
}

// Post-scan relations - typically used for bouncing down values from nested relations
void OsmLuaProcessing::postScanRelations() {
	if (!supportsPostScanRelations) return;

	for (const auto &relp : osmStore.scannedRelations.relationsForRelations) {
		reset();
		isPostScanRelation = true;
		RelationID id = relp.first;
		originalOsmID = id;
		currentPostScanTags = &(osmStore.scannedRelations.relation_tags(id));
		relationList = osmStore.scannedRelations.relations_for_relation_with_parents(id);
		luaState["relation_postscan_function"](this);
	}
}

bool OsmLuaProcessing::setNode(NodeID id, LatpLon node, const TagMap& tags) {
	reset();
	originalOsmID = id;
	lon = node.lon;
	latp= node.latp;
	currentTags = &tags;

	if (supportsReadingRelations && osmStore.scannedRelations.node_in_any_relations(id)) {
		relationList = osmStore.scannedRelations.relations_for_node(id);
	}

	//Start Lua processing for node
	try {
		luaState["node_function"]();
	} catch(luaProcessingException &e) {
		std::cerr << "Lua error on node " << originalOsmID << std::endl;
		exit(1);
	}

	if (!this->empty()) {
		TileCoordinates index = latpLon2index(node, osmMemTiles.getIndexZoom());

		for (auto &output : finalizeOutputs()) {
			osmMemTiles.addObjectToSmallIndex(index, output, originalOsmID);
		}

		return true;
	}

	return false;
}

// We are now processing a way
bool OsmLuaProcessing::setWay(WayID wayId, LatpLonVec const &llVec, const TagMap& tags) {
	reset();
	wayEmitted = false;
	originalOsmID = wayId;
	isWay = true;
	llVecPtr = &llVec;
	outerWayVecPtr = nullptr;
	innerWayVecPtr = nullptr;
	linestringInited = polygonInited = multiPolygonInited = false;

	if (supportsReadingRelations && osmStore.scannedRelations.way_in_any_relations(wayId)) {
		relationList = osmStore.scannedRelations.relations_for_way(wayId);
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
			kaguya::LuaRef ret = way_function();
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
void OsmLuaProcessing::setRelation(
	const std::vector<protozero::data_view>& stringTable,
	const PbfReader::Relation& relation,
	const WayVec& outerWayVec,
	const WayVec& innerWayVec,
	const TagMap& tags,
	bool isNativeMP, // only OSM type=multipolygon
	bool isInnerOuter // any OSM relation with "inner" and "outer" roles (e.g. type=multipolygon|boundary)
) {
	reset();
	this->stringTable = &stringTable;
	currentRelation = &relation;
	originalOsmID = relation.id;
	isWay = true;
	isRelation = true;
	isClosed = isNativeMP || isInnerOuter;

	llVecPtr = nullptr;
	outerWayVecPtr = &outerWayVec;
	innerWayVecPtr = &innerWayVec;
	currentTags = &tags;

	if (supportsReadingRelations && osmStore.scannedRelations.relation_in_any_relations(originalOsmID)) {
		relationList = osmStore.scannedRelations.relations_for_relation(originalOsmID);
	}

	// Start Lua processing for relation
	if (!isNativeMP && !supportsWritingRelations) return;
	try {
		if (isNativeMP && supportsWritingWays)
			luaState["way_function"]();
		else if (!isNativeMP && supportsWritingRelations)
			luaState["relation_function"]();
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

SignificantTags OsmLuaProcessing::GetSignificantNodeKeys() {
	if (!!luaState["node_keys"]) {
		std::vector<string> keys = luaState["node_keys"];
		return SignificantTags(keys);
	}

	return SignificantTags();
}

SignificantTags OsmLuaProcessing::GetSignificantWayKeys() {
	if (!!luaState["way_keys"]) {
		std::vector<string> keys = luaState["way_keys"];
		return SignificantTags(keys);
	}

	return SignificantTags();
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
