#include <iostream>
#include <unordered_set>
#include <cstring>

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

std::deque<std::mutex> vectorLayerMetadataMutexes;
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
bool rawHasTags() { return osmLuaProcessing->HasTags(); }
void rawSetTag(const std::string &key, const std::string &value) { return osmLuaProcessing->SetTag(key, value); }
// Native lua_CFunction versions of Holds/Find, bypassing kaguya's invoker.
// Semantics match the former kaguya-bound versions (KnownTagKey): kaguya does
// no type check for a single (non-overloaded) function, so the argument is
// read with lua_tolstring (numbers are coerced; other types give NULL/0 and
// simply miss).
static int luaHolds(lua_State* L) {
	size_t size = 0;
	const char* buffer = lua_tolstring(L, 1, &size);

	if (osmLuaProcessing->isPostScanRelation) {
		bool threw = false, rv = false;
		try {
			rv = osmLuaProcessing->Holds(std::string(buffer ? buffer : "", size));
		} catch (std::exception& e) {
			lua_pushstring(L, e.what());
			threw = true;
		}
		if (threw) return lua_error(L);
		lua_pushboolean(L, rv);
		return 1;
	}

	lua_pushboolean(L, osmLuaProcessing->currentTags->getKey(buffer, size) >= 0);
	return 1;
}

// Upvalue 1: the empty string, returned on a miss.
static int luaFind(lua_State* L) {
	size_t size = 0;
	const char* buffer = lua_tolstring(L, 1, &size);

	if (osmLuaProcessing->isPostScanRelation) {
		bool threw = false;
		try {
			const std::string rv = osmLuaProcessing->Find(std::string(buffer ? buffer : "", size));
			lua_pushlstring(L, rv.data(), rv.size());
		} catch (std::exception& e) {
			lua_pushstring(L, e.what());
			threw = true;
		}
		if (threw) return lua_error(L);
		return 1;
	}

	const TagMap* tags = osmLuaProcessing->currentTags;
	int64_t tagLoc = tags->getKey(buffer, size);
	if (tagLoc >= 0) {
		const protozero::data_view* value = tags->getValueFromKey(tagLoc);
		lua_pushlstring(L, value->data(), value->size());
	} else {
		lua_pushvalue(L, lua_upvalueindex(1));
	}
	return 1;
}

// Native lua_CFunction versions of Attribute*/MinZoom/ZOrder. Each wraps the
// kaguya binding registered under the same name (see wrapKaguyaBinding):
// upvalue 1 is kaguya's function tuple, upvalue 2 kaguya's closure. Calls whose
// arguments exactly match one overload (kaguya's strictCheckType, right arg
// count) are handled here; anything else goes to kaguya's own invoker on the
// same stack, so overload resolution, conversions and errors are unchanged.
static int kaguyaFallback(lua_State* L) {
	return lua_tocfunction(L, lua_upvalueindex(2))(L);
}

// Exceptions are reported as kaguya's invoker does
template<typename F> static int luaGuarded(lua_State* L, F&& fn) {
	try {
		fn();
		return 0;
	} catch (std::exception& e) {
		kaguya::util::traceBack(L, e.what());
	} catch (...) {
		kaguya::util::traceBack(L, "Unknown exception");
	}
	return lua_error(L);
}

template<typename T, unsigned Type> static int luaAttributeT(lua_State* L) {
	const int n = lua_gettop(L);
	if ((n != 2 && n != 3) ||
		!kaguya::lua_type_traits<std::string>::strictCheckType(L, 1) ||
		!kaguya::lua_type_traits<T>::strictCheckType(L, 2) ||
		(n == 3 && !kaguya::lua_type_traits<char>::strictCheckType(L, 3)))
		return kaguyaFallback(L);

	size_t keyLen = 0;
	const char* key = lua_tolstring(L, 1, &keyLen);
	const T val = kaguya::lua_type_traits<T>::get(L, 2);
	const char minzoom = n == 3 ? kaguya::lua_type_traits<char>::get(L, 3) : 0;
	return luaGuarded(L, [&]() { osmLuaProcessing->attributeFast(key, keyLen, val, minzoom, Type); });
}

static int luaMinZoom(lua_State* L) {
	if (lua_type(L, 1) != LUA_TNUMBER) return kaguyaFallback(L);
	const double z = kaguya::lua_type_traits<double>::get(L, 1);
	return luaGuarded(L, [&]() { osmLuaProcessing->MinZoom(z); });
}

static int luaZOrder(lua_State* L) {
	if (lua_type(L, 1) != LUA_TNUMBER) return kaguyaFallback(L);
	const double z = kaguya::lua_type_traits<double>::get(L, 1);
	return luaGuarded(L, [&]() { osmLuaProcessing->ZOrder(z); });
}

// Replace the kaguya-bound global `name` with native function fn, keeping
// kaguya's closure and function tuple as upvalues for kaguyaFallback.
static void wrapKaguyaBinding(lua_State* L, const char* name, lua_CFunction fn) {
	lua_getglobal(L, name);
	lua_getupvalue(L, -1, 1);
	lua_pushvalue(L, -2);
	lua_pushcclosure(L, fn, 2);
	lua_setglobal(L, name);
	lua_pop(L, 1);
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
void rawModifyId(const int newId) { return osmLuaProcessing->ModifyId(newId); }
void rawLayer(const std::string& layerName, bool area) { return osmLuaProcessing->Layer(layerName, area); }
void rawLayerAsCentroid(const std::string &layerName, kaguya::VariadicArgType nodeSources) { return osmLuaProcessing->LayerAsCentroid(layerName, nodeSources); }
void rawMinZoom(const double z) { return osmLuaProcessing->MinZoom(z); }
void rawZOrder(const double z) { return osmLuaProcessing->ZOrder(z); }
void rawScore(const double score) { return osmLuaProcessing->Score(score); }
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

// Same setup as kaguya::State's owning constructor (and luaL_newstate), but with LuaPool
// as the allocator: kaguya's allocator interface drops osize, which the pool needs.
#if !defined(LUAJIT) && LUA_VERSION_NUM <= 504
#define TM_LUA_POOL 1
#endif

#if defined(TM_LUA_POOL) && LUA_VERSION_NUM == 504
// Lua 5.4 luaL_newstate() also installs these (static in lauxlib.c): warnings default off
static void tmLuaWarnfOff(void *ud, const char *message, int tocont);
static void tmLuaWarnfOn(void *ud, const char *message, int tocont);
static void tmLuaWarnfCont(void *ud, const char *message, int tocont);
static int tmLuaWarnCheckControl(lua_State *L, const char *message, int tocont) {
	if (tocont || *(message++) != '@') return 0;
	if (strcmp(message, "off") == 0) lua_setwarnf(L, tmLuaWarnfOff, L);
	else if (strcmp(message, "on") == 0) lua_setwarnf(L, tmLuaWarnfOn, L);
	return 1;
}
static void tmLuaWarnfOff(void *ud, const char *message, int tocont) {
	tmLuaWarnCheckControl((lua_State *)ud, message, tocont);
}
static void tmLuaWarnfCont(void *ud, const char *message, int tocont) {
	lua_State *L = (lua_State *)ud;
	lua_writestringerror("%s", message);
	if (tocont) lua_setwarnf(L, tmLuaWarnfCont, L);
	else {
		lua_writestringerror("%s", "\n");
		lua_setwarnf(L, tmLuaWarnfOn, L);
	}
}
static void tmLuaWarnfOn(void *ud, const char *message, int tocont) {
	if (tmLuaWarnCheckControl((lua_State *)ud, message, tocont)) return;
	lua_writestringerror("%s", "Lua warning: ");
	tmLuaWarnfCont(ud, message, tocont);
}
#endif

// Copies of kaguya::State's private initializing_panic/default_panic/stderror_out
static int tmLuaInitializingPanic(lua_State *L) {
	kaguya::ErrorHandler::throwDefaultError(lua_status(L), lua_tostring(L, -1));
	return 0;
}
static int tmLuaDefaultPanic(lua_State *L) {
	if (kaguya::ErrorHandler::handle(lua_status(L), L)) return 0;
	fprintf(stderr, "PANIC: unprotected error in call to Lua API (%s)\n", lua_tostring(L, -1));
	fflush(stderr);
	return 0;
}
static void tmLuaStderrorOut(int status, const char* message) {
	std::cerr << message << std::endl;
}

lua_State *OsmLuaProcessing::newLuaState(LuaPool &pool) {
#ifdef TM_LUA_POOL
	lua_State *L = lua_newstate(&LuaPool::alloc, &pool);
#if LUA_VERSION_NUM == 504
	if (L) lua_setwarnf(L, tmLuaWarnfOff, L);
#endif
	if (!L) L = luaL_newstate(); // e.g. LuaJIT not detected as such: behave like base
#else
	lua_State *L = luaL_newstate();
#endif
	if (!L) return L;
	// kaguya::State::init(AllLoadLibs())
	lua_atpanic(L, &tmLuaInitializingPanic);
	try {
		if (!kaguya::ErrorHandler::getHandler(L)) {
			kaguya::util::ScopedSavedStack save(L);
			kaguya::ErrorHandler::registerHandler(L, &tmLuaStderrorOut);
		}
		luaL_openlibs(L);
		lua_atpanic(L, &tmLuaDefaultPanic);
	} catch (const kaguya::LuaException&) {
		lua_close(L);
		L = nullptr;
	}
	return L;
}

OsmLuaProcessing::OsmLuaProcessing(
	OSMStore &osmStore,
	const class Config &configIn,
	class LayerDefinition &layers,
	const string &luaFile,
	const class ShpMemTiles &shpMemTiles, 
	class OsmMemTiles &osmMemTiles,
	AttributeStore &attributeStore,
	class Declutter &declutter,
	bool materializeGeometries,
	bool isFirst) :
	osmStore(osmStore),
	luaStateHolder(newLuaState(luaPool)),
	luaState(luaStateHolder.L),
	shpMemTiles(shpMemTiles),
	osmMemTiles(osmMemTiles),
	attributeStore(attributeStore),
	declutter(declutter),
	config(configIn),
	currentTags(NULL),
	layers(layers),
	materializeGeometries(materializeGeometries) {

	sigusr1Handler.initialize();
	if (vectorLayerMetadataMutexes.size() <= layers.layers.size()) {
		vectorLayerMetadataMutexes.resize(layers.layers.size() + 1);
	}

	// ----	Initialise Lua
	g_luaState = &luaState;
	luaState.setErrorHandler(lua_error_handler);
	luaState.dofile(luaFile.c_str());

	osmLuaProcessing = this;
	luaState["Id"] = &rawId;
	luaState["OsmType"] = &rawOsmType;
	luaState["AllKeys"] = &rawAllKeys;
	luaState["AllTags"] = &rawAllTags;
	lua_State* L = luaState.state();
	lua_pushcclosure(L, luaHolds, 0);
	lua_setglobal(L, "Holds");
	lua_pushlstring(L, "", 0);
	lua_pushcclosure(L, luaFind, 1);
	lua_setglobal(L, "Find");
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
	luaState["ModifyId"] = &rawModifyId;
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
	wrapKaguyaBinding(L, "Attribute", luaAttributeT<protozero::data_view, 0>);
	wrapKaguyaBinding(L, "AttributeNumeric", luaAttributeT<double, 1>);
	wrapKaguyaBinding(L, "AttributeBoolean", luaAttributeT<bool, 2>);
	wrapKaguyaBinding(L, "AttributeInteger", luaAttributeT<int, 3>);
	wrapKaguyaBinding(L, "MinZoom", luaMinZoom);
	wrapKaguyaBinding(L, "ZOrder", luaZOrder);
	luaState["Score"] = &rawScore;
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

template <typename DstRing, typename SrcRing>
void projectRing(DstRing& dst, const SrcRing& src) {
	dst.resize(src.size());
	for (std::size_t i = 0; i < src.size(); ++i) {
		geom::set<0>(dst[i], geom::get<0>(src[i]));
		geom::set<1>(dst[i], latp2lat(geom::get<1>(src[i])));
	}
}

#if BOOST_VERSION >= 106700
double OsmLuaProcessing::projectedPolygonArea(const Polygon &p) {
	areaPolygonCache.inners().resize(p.inners().size());
	projectRing(areaPolygonCache.outer(), p.outer());
	for (std::size_t i = 0; i < p.inners().size(); ++i) {
		projectRing(areaPolygonCache.inners()[i], p.inners()[i]);
	}

	geom::strategy::area::spherical<> sph_strategy(RadiusMeter);
	return geom::area(areaPolygonCache, sph_strategy);
}
#endif

// Returns area
double OsmLuaProcessing::Area() {
	if (!IsClosed()) return 0;

#if BOOST_VERSION >= 106700
	if (isRelation) {
		// Boost won't calculate area of a multipolygon, so we just total up the member polygons
		double totalArea = 0;
		const MultiPolygon &mp = multiPolygonCached();
		for (MultiPolygon::const_iterator it = mp.begin(); it != mp.end(); ++it) {
			totalArea += projectedPolygonArea(*it);
		}
		return totalArea;
	} else if (isWay) {
		return projectedPolygonArea(polygonCached());
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
		// Same as osmStore.llListLinestring(), refilling the cache in place
		linestringCache.clear();
		linestringCache.reserve(llVecPtr->size());
		for (const LatpLon &ll : *llVecPtr)
			linestringCache.push_back(geom::make<Point>(ll.lon/10000000.0, ll.latp/10000000.0));
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
			noteIfDecluttered(LatpLon { latp, lon });
			return;
		}
		else if (geomType==POLYGON_) {
			// polygon

			if (isRelation) {
				try {
					MultiPolygon mp = multiPolygonCached();
					auto correctionResult = CorrectGeometry(mp);
					if(correctionResult == CorrectGeometryResult::Invalid) return;
					// make_valid usually succeeds; checking that once here saves
					// re-validating the relation in every tile it is clipped to.
					const bool knownValid = correctionResult == CorrectGeometryResult::Valid ||
						PrecheckValid(mp) || geom::is_valid(mp);
					NodeID id = osmMemTiles.storeMultiPolygon(mp, knownValid);
					OutputObject oo(geomType, layers.layerMap[layerName], id, 0, layerMinZoom);
					outputs.push_back(std::make_pair(std::move(oo), attributes));
				} catch(std::out_of_range &err) {
					cout << "In relation " << originalOsmID << ": " << err.what() << endl;
					return;
				}
			}
			else if (isWay) {
				//Is there a more efficient way to do this?
				const Linestring &ls = linestringCached();
				// Reused scratch: a single polygon with ls as its outer ring and no inners
				MultiPolygon &mp = layerScratchMultiPolygon;
				mp.resize(1);
				mp[0].outer().assign(ls.begin(), ls.end());
				mp[0].inners().clear();

				auto correctionResult = CorrectGeometry(mp, /*allowReverse=*/isWay);
				if(correctionResult == CorrectGeometryResult::Invalid) return;
				NodeID id = 0;
				if (!materializeGeometries && correctionResult == CorrectGeometryResult::Valid) {
					id = USE_WAY_STORE | originalOsmID;
					wayEmitted = true;
				} else if (!materializeGeometries && correctionResult == CorrectGeometryResult::Reversed) {
					// Only defect was ring orientation: rebuild from the way store, reversed.
					id = USE_WAY_STORE_REVERSED | originalOsmID;
					wayEmitted = true;
				} else 
					id = osmMemTiles.storeMultiPolygon(mp, correctionResult != CorrectGeometryResult::Corrected);
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
			// Check the cached linestring in place; copy it only if it needs correcting
			const Linestring &cachedLs = linestringCached();
			geom::validity_failure_type failure = CheckGeometry(cachedLs);
			Linestring correctedLs;
			auto correctionResult = CorrectGeometryResult::Valid;
			if (failure != geom::validity_failure_type::no_failure) {
				correctedLs = cachedLs;
				correctionResult = CorrectGeometryAfterCheck(correctedLs, failure);
			}
			const Linestring &ls = failure != geom::validity_failure_type::no_failure ? correctedLs : cachedLs;
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
	noteIfDecluttered(LatpLon { (int32_t)geomp.y(), (int32_t)geomp.x() });
}

// If the layer we've just written a point to is decluttered, remember where the point is:
// it'll be held back from the tile index until all features have been read and ranked.
void OsmLuaProcessing::noteIfDecluttered(LatpLon point) {
	if (!declutter.isDecluttered(outputs.back().first.layer)) return;
	declutterOutputs.push_back({ (uint32_t)(outputs.size() - 1), point, 0 });
}

Point OsmLuaProcessing::calculateCentroid(CentroidAlgorithm algorithm) {
	// Memoized per object: profiles often ask for the same centroid more than once
	// (e.g. housenumber and POI on the same building). Exceptions are not memoized.
	const int a = static_cast<int>(algorithm);
	if (centroidMemoValid[a]) {
		return centroidMemo[a];
	}
	centroidMemo[a] = calculateCentroidUncached(algorithm);
	centroidMemoValid[a] = true;
	return centroidMemo[a];
}

Point OsmLuaProcessing::calculateCentroidUncached(CentroidAlgorithm algorithm) {
	Point centroid;
	if (isRelation) {
		const MultiPolygon &tmp = multiPolygonCached();

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
		if (!isClosed) {
			const Linestring &ls = linestringCached();
			if (ls.empty())
				throw geom::centroid_exception();
			if (ls.size()==1) {
				centroid = ls.front();
			} else {
				geom::centroid(ls, centroid);
			}
		} else {
			const Linestring &ls = linestringCached();
			// Reused scratch polygon: ls as the outer ring, no inners
			Polygon &p = centroidScratchPolygon;
			p.outer().assign(ls.begin(), ls.end());
			p.inners().clear();

			if (algorithm == CentroidAlgorithm::Polylabel) {
				// CONSIDER: pick precision intelligently
				centroid = mapbox::polylabel(p);
			} else {
				geom::centroid(p, centroid);
			}
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

void OsmLuaProcessing::removeAttributeIfNeeded(uint16_t keyIndex) {
	// Does it exist?
	for (int i = 0; i < outputKeys.size(); i++) {
		if (outputKeys[i] == keyIndex) {
			AttributeSet& set = outputs.back().second;
			set.removePairWithKey(attributeStore.pairStore, keyIndex);
			return;
		}
	}

	outputKeys.push_back(keyIndex);
}

// Force a new ID
void OsmLuaProcessing::ModifyId(const int newId) {
    originalOsmID = static_cast<int64_t>(newId);
}

// Set attributes in a vector tile's Attributes table
void OsmLuaProcessing::Attribute(const string &key, const protozero::data_view val, const char minzoom) {
	if (outputs.size()==0) { ProcessingError("Can't add Attribute if no Layer set"); return; }
	const uint16_t keyIndex = attributeStore.keyStore.key2index(key);
	removeAttributeIfNeeded(keyIndex);
	attributeStore.addAttributeWithKeyIndex(outputs.back().second, keyIndex, key, val, minzoom);
	setVectorLayerMetadata(outputs.back().first.layer, key, 0);
}

void OsmLuaProcessing::AttributeNumeric(const string &key, const double val, const char minzoom) {
	if (outputs.size()==0) { ProcessingError("Can't add Attribute if no Layer set"); return; }
	const uint16_t keyIndex = attributeStore.keyStore.key2index(key);
	removeAttributeIfNeeded(keyIndex);
	attributeStore.addAttributeWithKeyIndex(outputs.back().second, keyIndex, val, minzoom);
	setVectorLayerMetadata(outputs.back().first.layer, key, 1);
}

void OsmLuaProcessing::AttributeBoolean(const string &key, const bool val, const char minzoom) {
	if (outputs.size()==0) { ProcessingError("Can't add Attribute if no Layer set"); return; }
	const uint16_t keyIndex = attributeStore.keyStore.key2index(key);
	removeAttributeIfNeeded(keyIndex);
	attributeStore.addAttributeWithKeyIndex(outputs.back().second, keyIndex, val, minzoom);
	setVectorLayerMetadata(outputs.back().first.layer, key, 2);
}

void OsmLuaProcessing::AttributeInteger(const string &key, const int val, const char minzoom) {
	if (outputs.size()==0) { ProcessingError("Can't add Attribute if no Layer set"); return; }
	const uint16_t keyIndex = attributeStore.keyStore.key2index(key);
	removeAttributeIfNeeded(keyIndex);
	attributeStore.addAttributeWithKeyIndex(outputs.back().second, keyIndex, val, minzoom);
	setVectorLayerMetadata(outputs.back().first.layer, key, 3);
}

// Set minimum zoom
void OsmLuaProcessing::MinZoom(const double z) {
	if (outputs.size()==0) { ProcessingError("Can't set minimum zoom if no Layer set"); return; }
	outputs.back().first.setMinZoom(z);
}

// Set the score used to rank this feature against its neighbours when decluttering
void OsmLuaProcessing::Score(const double score) {
	if (outputs.size()==0) { ProcessingError("Can't set score if no Layer set"); return; }
	if (declutterOutputs.empty() || declutterOutputs.back().outputIndex != outputs.size()-1) {
		ProcessingError("Score() only applies to point features in a layer with declutter_below set");
		return;
	}
	declutterOutputs.back().score = OutputObject::finite_cast<int32_t>(score);
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
thread_local std::vector<std::unordered_set<std::string>> alreadySeen;

void OsmLuaProcessing::setVectorLayerMetadata(const uint_least8_t layer, const string &key, const uint type) {
	if (alreadySeen.size() <= layer) alreadySeen.resize(layer + 1);
	if (alreadySeen[layer].count(key)) return;
	std::lock_guard<std::mutex> lock(vectorLayerMetadataMutexes[layer]);
	layers.layers[layer].attributeMap[key] = type;
	alreadySeen[layer].insert(key);
}

// Per-thread cache of attribute keys, indexed by the address of the Lua
// string buffer. A hit needs the same address, length and bytes, so a reused
// address or a non-interned string simply misses. seenLayers caches, per
// layer, that setVectorLayerMetadata has already run for this key on this
// thread (alreadySeen is thread_local and never cleared, so a set bit stays
// true; an evicted entry just starts again with no bits set).
namespace {
struct AttrKeyCacheEntry {
	const char* ptr = nullptr;
	size_t len = 0;
	std::string key;
	uint16_t keyIndex = 0;
	std::vector<uint64_t> seenLayers;
};
const size_t ATTR_KEY_CACHE_SIZE = 512;
thread_local std::vector<AttrKeyCacheEntry> attrKeyCache;
thread_local const AttributeKeyStore* attrKeyCacheStore = nullptr;

AttrKeyCacheEntry& lookupAttrKey(AttributeKeyStore& keyStore, const char* key, size_t keyLen) {
	if (attrKeyCacheStore != &keyStore) {
		attrKeyCache.assign(ATTR_KEY_CACHE_SIZE, AttrKeyCacheEntry());
		attrKeyCacheStore = &keyStore;
	}
	const uintptr_t p = reinterpret_cast<uintptr_t>(key);
	AttrKeyCacheEntry& e = attrKeyCache[((p >> 4) ^ (p >> 13)) & (ATTR_KEY_CACHE_SIZE - 1)];
	if (e.ptr == key && e.len == keyLen && e.key.size() == keyLen && memcmp(e.key.data(), key, keyLen) == 0) {
		return e;
	}
	// resolve the index first: key2index may throw (too many keys), and a pcall in Lua could continue
	std::string k(key, keyLen);
	const uint16_t idx = keyStore.key2index(k);
	e.key.swap(k);
	e.keyIndex = idx;
	e.ptr = key;
	e.len = keyLen;
	e.seenLayers.clear();
	return e;
}

inline void addAttributeByIndex(AttributeStore& store, AttributeSet& set, const AttrKeyCacheEntry& e, const protozero::data_view val, char minzoom) {
	store.addAttributeWithKeyIndex(set, e.keyIndex, e.key, val, minzoom);
}
template<typename T> inline void addAttributeByIndex(AttributeStore& store, AttributeSet& set, const AttrKeyCacheEntry& e, T val, char minzoom) {
	store.addAttributeWithKeyIndex(set, e.keyIndex, val, minzoom);
}
}

// Same as Attribute/AttributeNumeric/AttributeBoolean/AttributeInteger, with
// the key index and vector_layers bookkeeping served from the key cache.
template<typename T>
void OsmLuaProcessing::attributeFast(const char *key, size_t keyLen, T val, char minzoom, uint type) {
	if (outputs.size()==0) { ProcessingError("Can't add Attribute if no Layer set"); return; }
	AttrKeyCacheEntry& e = lookupAttrKey(attributeStore.keyStore, key, keyLen);
	removeAttributeIfNeeded(e.keyIndex);
	addAttributeByIndex(attributeStore, outputs.back().second, e, val, minzoom);

	const uint_least8_t layer = outputs.back().first.layer;
	const size_t word = layer / 64;
	const uint64_t bit = uint64_t(1) << (layer % 64);
	if (e.seenLayers.size() > word && (e.seenLayers[word] & bit)) {
		return;
	}
	setVectorLayerMetadata(layer, e.key, type);
	if (e.seenLayers.size() <= word) e.seenLayers.resize(word + 1, 0);
	e.seenLayers[word] |= bit;
}

// Call global Lua function `name` with no arguments, as kaguya does for a
// function value (its pcall has no message handler; on error the registered
// error handler gets the status and message). Results are discarded.
// keepGlobals: luaState[name]() keeps the globals table on the stack below the
// call (visible in the error handler's stack dump); calling through a
// kaguya::LuaFunction doesn't.
bool OsmLuaProcessing::callLuaGlobal(const char *name, bool keepGlobals) {
	lua_State* L = luaState.state();
	const int top = lua_gettop(L);
#if LUA_VERSION_NUM >= 502
	lua_pushglobaltable(L);
#else
	lua_pushvalue(L, LUA_GLOBALSINDEX);
#endif
	lua_getfield(L, -1, name);
	if (!keepGlobals) lua_remove(L, -2);
	if (lua_type(L, -1) != LUA_TFUNCTION) {
		lua_settop(L, top);
		return false;
	}
	const int status = lua_pcall(L, 0, 0, 0);
	kaguya::except::checkErrorAndThrow(status, L);
	lua_settop(L, top);
	return true;
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
		if (!callLuaGlobal("node_function", true)) luaState["node_function"]();
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
	centroidMemoValid[0] = centroidMemoValid[1] = false;

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
			if (!callLuaGlobal("way_function", false)) {
				kaguya::LuaFunction way_function = luaState["way_function"];
				kaguya::LuaRef ret = way_function();
				assert(!ret);
			}
		} catch(luaProcessingException &e) {
			std::cerr << "Lua error on way " << originalOsmID << std::endl;
			exit(1);
		}
	}

	if (!this->empty()) {
		std::vector<OutputObject> &objects = finalizeOutputs();
		if (!objects.empty()) osmMemTiles.addGeometryToIndex(linestringCached(), objects, originalOsmID);
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
		if (isNativeMP && supportsWritingWays) {
			if (!callLuaGlobal("way_function", true)) luaState["way_function"]();
		} else if (!isNativeMP && supportsWritingRelations) {
			if (!callLuaGlobal("relation_function", true)) luaState["relation_function"]();
		}
	} catch(luaProcessingException &e) {
		std::cerr << "Lua error on relation " << originalOsmID << std::endl;
		exit(1);
	}
	if (this->empty()) return;

	try {
		std::vector<OutputObject> &objects = finalizeOutputs();
		if (objects.empty()) return;
		if (isClosed) {
			osmMemTiles.addGeometryToIndex(multiPolygonCached(), objects, originalOsmID);
		} else {
			osmMemTiles.addGeometryToIndex(multiLinestringCached(), objects, originalOsmID);
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


std::vector<OutputObject> &OsmLuaProcessing::finalizeOutputs() {
	std::vector<OutputObject> &list = finalizedOutputs;
	list.clear();
	list.reserve(this->outputs.size());
	size_t nextDeclutter = 0;
	for (size_t i = 0; i < this->outputs.size(); i++) {
		auto& output = this->outputs[i];
		output.first.setAttributeSet(attributeStore.add(output.second));
		if (nextDeclutter < declutterOutputs.size() && declutterOutputs[nextDeclutter].outputIndex == i) {
			// Park it: Declutter::apply() will set its minimum zoom and index it later
			const auto& pending = declutterOutputs[nextDeclutter++];
			declutter.add(output.first, pending.point, originalOsmID, pending.score, false);
			continue;
		}
		list.push_back(output.first);
	}
	return list;
}
