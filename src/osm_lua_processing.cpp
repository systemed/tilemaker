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

std::string rawId() { if (osmLuaProcessing->memoTrace.active) osmLuaProcessing->memoLuaOnly(); return osmLuaProcessing->Id(); }
std::string rawOsmType() { if (osmLuaProcessing->memoTrace.active) osmLuaProcessing->memoLuaOnly(); return osmLuaProcessing->OsmType(); }
kaguya::LuaTable rawAllKeys() { if (osmLuaProcessing->memoTrace.active) osmLuaProcessing->memoLuaOnly();
	if (osmLuaProcessing->isPostScanRelation) {
		return osmLuaProcessing->AllKeys(*g_luaState);
	}

	auto tags = osmLuaProcessing->currentTags->exportToBoostMap();

	return getAllKeys(*g_luaState, &tags);
}kaguya::LuaTable rawAllTags() { if (osmLuaProcessing->memoTrace.active) osmLuaProcessing->memoLuaOnly();
	if (osmLuaProcessing->isPostScanRelation) {
		return osmLuaProcessing->AllTags(*g_luaState);
	}

	auto tags = osmLuaProcessing->currentTags->exportToBoostMap();

	return getAllTags(*g_luaState, &tags);
}
bool rawHasTags() { if (osmLuaProcessing->memoTrace.active) osmLuaProcessing->memoLuaOnly(); return osmLuaProcessing->HasTags(); }
void rawSetTag(const std::string &key, const std::string &value) { if (osmLuaProcessing->memoTrace.active) osmLuaProcessing->memoLuaOnly(); return osmLuaProcessing->SetTag(key, value); }
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

	const bool held = osmLuaProcessing->currentTags->getKey(buffer, size) >= 0;
	if (osmLuaProcessing->memoTrace.active) osmLuaProcessing->memoRead(OsmLuaProcessing::MemoNode::Holds, buffer, size, held ? "1" : "0");
	lua_pushboolean(L, held);
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
		if (osmLuaProcessing->memoTrace.active) {
			if (osmLuaProcessing->memoValueTag(buffer, size)) osmLuaProcessing->memoReadValue(buffer, size, std::string(value->data(), value->size()));
			else osmLuaProcessing->memoRead(OsmLuaProcessing::MemoNode::Find, buffer, size, std::string(value->data(), value->size()));
		}
		lua_pushlstring(L, value->data(), value->size());
	} else {
		if (osmLuaProcessing->memoTrace.active) {
			if (osmLuaProcessing->memoValueTag(buffer, size)) osmLuaProcessing->memoReadValue(buffer, size, std::string());
			else osmLuaProcessing->memoRead(OsmLuaProcessing::MemoNode::Find, buffer, size, std::string());
		}
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

std::vector<std::string> rawFindIntersecting(const std::string &layerName) { if (osmLuaProcessing->memoTrace.active) osmLuaProcessing->memoLuaOnly(); return osmLuaProcessing->FindIntersecting(layerName); }
bool rawIntersects(const std::string& layerName) { if (osmLuaProcessing->memoTrace.active) osmLuaProcessing->memoLuaOnly(); return osmLuaProcessing->Intersects(layerName); }
std::vector<std::string> rawFindCovering(const std::string& layerName) { if (osmLuaProcessing->memoTrace.active) osmLuaProcessing->memoLuaOnly(); return osmLuaProcessing->FindCovering(layerName); }
bool rawCoveredBy(const std::string& layerName) { if (osmLuaProcessing->memoTrace.active) osmLuaProcessing->memoLuaOnly(); return osmLuaProcessing->CoveredBy(layerName); }
bool rawIsClosed() {
	const bool closed = osmLuaProcessing->IsClosed();
	if (osmLuaProcessing->memoTrace.active) osmLuaProcessing->memoRead(OsmLuaProcessing::MemoNode::IsClosed, "", 0, closed ? "1" : "0");
	return closed;
}
bool rawIsMultiPolygon() { if (osmLuaProcessing->memoTrace.active) osmLuaProcessing->memoLuaOnly(); return osmLuaProcessing->IsMultiPolygon(); }
double rawMercatorArea() {
	const double area = osmLuaProcessing->MercatorArea();
	if (osmLuaProcessing->memoTrace.active) {
		if (osmLuaProcessing->memoArea.empty()) osmLuaProcessing->memoLuaOnly();
		else osmLuaProcessing->memoRead(OsmLuaProcessing::MemoNode::MercatorArea, "", 0, osmLuaProcessing->memoAreaResult(area));
	}
	return area;
}
void rawMinZoomByArea(double limit, std::vector<double> thresholds) { return osmLuaProcessing->MinZoomByArea(limit, thresholds); }
double rawArea() { if (osmLuaProcessing->memoTrace.active) osmLuaProcessing->memoLuaOnly(); return osmLuaProcessing->Area(); }
double rawLength() { if (osmLuaProcessing->memoTrace.active) osmLuaProcessing->memoLuaOnly(); return osmLuaProcessing->Length(); }
kaguya::optional<std::vector<double>> rawCentroid(kaguya::VariadicArgType algorithm) { if (osmLuaProcessing->memoTrace.active) osmLuaProcessing->memoLuaOnly(); return osmLuaProcessing->Centroid(algorithm); }
void rawModifyId(const int newId) { return osmLuaProcessing->ModifyId(newId); }
void rawLayer(const std::string& layerName, bool area) { return osmLuaProcessing->Layer(layerName, area); }
void rawLayerAsCentroid(const std::string &layerName, kaguya::VariadicArgType nodeSources) { return osmLuaProcessing->LayerAsCentroid(layerName, nodeSources); }
void rawMinZoom(const double z) { return osmLuaProcessing->MinZoom(z); }
void rawZOrder(const double z) { return osmLuaProcessing->ZOrder(z); }
void rawScore(const double score) { return osmLuaProcessing->Score(score); }
OsmLuaProcessing::OptionalRelation rawNextRelation() {
	OsmLuaProcessing::OptionalRelation r = osmLuaProcessing->NextRelation();
	if (osmLuaProcessing->memoTrace.active) {
		// way_memo_relations: the read is whether there is a relation, and its role
		osmLuaProcessing->memoRead(OsmLuaProcessing::MemoNode::NextRelation, "", 0, r.done ? "0" : osmLuaProcessing->memoRelations ? "1" + r.role : "1");
		if (!r.done && !osmLuaProcessing->memoRelations) osmLuaProcessing->memoLuaOnly();
	}
	return r;
}
void rawRestartRelations() { if (osmLuaProcessing->memoTrace.active) osmLuaProcessing->memoLuaOnly(); return osmLuaProcessing->RestartRelations(); }
std::string rawFindInRelation(const std::string& key) {
	if (osmLuaProcessing->memoTrace.active && !(osmLuaProcessing->memoRelations && osmLuaProcessing->relationCurrent())) osmLuaProcessing->memoLuaOnly();
	std::string value = osmLuaProcessing->FindInRelation(key);
	if (osmLuaProcessing->memoTrace.active) osmLuaProcessing->memoRead(OsmLuaProcessing::MemoNode::FindInRelation, key.data(), key.size(), value);
	return value;
}
void rawAccept() { if (osmLuaProcessing->memoTrace.active) osmLuaProcessing->memoLuaOnly(); return osmLuaProcessing->Accept(); }
double rawAreaIntersecting(const std::string& layerName) { if (osmLuaProcessing->memoTrace.active) osmLuaProcessing->memoLuaOnly(); return osmLuaProcessing->AreaIntersecting(layerName); }

void rawSetData(const std::string &key, const std::string &value) { if (osmLuaProcessing->memoTrace.active) osmLuaProcessing->memoLuaOnly(); 
	std::lock_guard<std::mutex> lock(osmLuaProcessing->dataStoreMutex);
	osmLuaProcessing->dataStore[key] = value;
}
std::string rawGetData(const std::string &key) { if (osmLuaProcessing->memoTrace.active) osmLuaProcessing->memoLuaOnly();
	auto r = osmLuaProcessing->dataStore.find(key);
	return r==osmLuaProcessing->dataStore.end() ? "" : r->second;
}

static std::unordered_map<std::string, std::vector<std::string>> readMemoValueTags(lua_State *L, const char *global, std::unordered_map<std::string, std::string> &lengths);
static std::vector<double> readMemoArea(lua_State *L, const char *global);

// Memo of way_function: the processors that use it (for printMemoStats)
static std::mutex memoInstancesMutex;
static std::vector<OsmLuaProcessing*> memoInstances;
static uint64_t memoDone[OsmLuaProcessing::MEMO_KINDS][4] = {};	// of processors gone, by MemoKind: hits, recorded, through Lua, tree nodes

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

// Register the functions a profile can call as Lua globals.
static void registerLuaFunctions(kaguya::State &luaState) {
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
	luaState["MercatorArea"] = &rawMercatorArea;
	luaState["MinZoomByArea"] = &rawMinZoomByArea;
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
	osmLuaProcessing = this;
	// The built-in functions are registered before the profile is loaded, so that it can keep them in
	// locals at the top (local Find, Layer = Find, Layer), and again afterwards, so that they still
	// replace profile functions of the same name, as before.
	registerLuaFunctions(luaState);
	luaState.dofile(luaFile.c_str());
	registerLuaFunctions(luaState);

	supportsRemappingShapefiles = !!luaState["attribute_function"];
	supportsReadingRelations    = !!luaState["relation_scan_function"];
	supportsPostScanRelations   = !!luaState["relation_postscan_function"];
	supportsWritingNodes        = !!luaState["node_function"];
	supportsWritingWays         = !!luaState["way_function"];
	supportsWritingRelations    = !!luaState["relation_function"];
	memoEnabled                 = !!luaState["way_memo"];
	if (memoEnabled) {
		memoValueTags = readMemoValueTags(luaState.state(), "way_memo_values", memoValueLengths);
		memoArea = readMemoArea(luaState.state(), "way_memo_area");
		memoRelations = !!luaState["way_memo_relations"];
	}
	nodeMemoEnabled = supportsWritingNodes && !!luaState["node_memo"];
	MemoState &nodeMemo = memoSaved[MemoNodes];
	nodeMemo.enabled = nodeMemoEnabled;
	if (nodeMemoEnabled) {
		nodeMemo.valueTags = readMemoValueTags(luaState.state(), "node_memo_values", nodeMemo.valueLengths);
		nodeMemo.area = readMemoArea(luaState.state(), "node_memo_area");
		nodeMemo.relations = !!luaState["node_memo_relations"];
	}
	// Multipolygon relations run way_function: its memo, with a tree of their own
	MemoState &relationMemo = memoSaved[MemoRelations];
	relationMemo.enabled = memoEnabled;
	relationMemo.valueTags = memoValueTags;
	relationMemo.valueLengths = memoValueLengths;
	relationMemo.area = memoArea;
	relationMemo.relations = memoRelations;
	if (memoEnabled || nodeMemoEnabled) {
		std::lock_guard<std::mutex> lock(memoInstancesMutex);
		memoInstances.push_back(this);
	}

	// ---- Call init_function of Lua logic

	if (!!luaState["init_function"]) {
		luaState["init_function"](this->config.projectName, isFirst);
	}
}

OsmLuaProcessing::~OsmLuaProcessing() {
	{
		std::lock_guard<std::mutex> lock(memoInstancesMutex);
		auto it = std::find(memoInstances.begin(), memoInstances.end(), this);
		if (it != memoInstances.end()) {
			memoInstances.erase(it);
			memoCounts(memoDone);
		}
	}
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

// Geometries are kept in Web Mercator (lon, latp in degrees), so the planar area only needs scaling
double OsmLuaProcessing::MercatorArea() {
	if (!IsClosed()) return 0;
	const double metresPerDegree = 6378137.0 * M_PI / 180.0;	// Web Mercator, at the equator
	double area = 0;
	if (isRelation) {
		for (const Polygon &p : multiPolygonCached()) area += std::abs(geom::area(p));
	} else if (isWay) {
		area = std::abs(geom::area(polygonCached()));
	}
	return area * metresPerDegree * metresPerDegree;
}

void OsmLuaProcessing::MinZoomByArea(double limit, const std::vector<double> &thresholds) {
	if (memoTrace.active) {
		MemoOp op{ MemoOp::MinZoomByArea, false, 0, limit, std::string(), std::string() };
		op.numbers = thresholds;
		memoOp(std::move(op));
	}
	if (outputs.size()==0) { ProcessingError("Can't set minimum zoom if no Layer set"); return; }
	const double area = MercatorArea();
	double z = thresholds.size() + 1;
	for (std::size_t i = 0; i < thresholds.size(); i++) {
		if (limit <= i + 1 && area > thresholds[i]) { z = i + 1; break; }
	}
	outputs.back().first.setMinZoom(z);
}

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
		const size_t n = llVecPtr->size();
		linestringCache.resize(n);
		const LatpLon *src = llVecPtr->data();
		Point *dst = linestringCache.data();
		for (size_t i = 0; i < n; i++)
			dst[i] = geom::make<Point>(src[i].lon/10000000.0, src[i].latp/10000000.0);
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
		// As osmStore.llListPolygon(), refilling the cache in place: the points of
		// linestringCached(), then corrected
		auto &outer = polygonCache.outer();
		polygonCache.inners().clear();
		if (linestringInited) outer.assign(linestringCache.begin(), linestringCache.end());
		else {
			outer.clear();
			outer.reserve(llVecPtr->size());
			for (const LatpLon &ll : *llVecPtr)
				outer.push_back(geom::make<Point>(ll.lon/10000000.0, ll.latp/10000000.0));
		}
		geom::correct(polygonCache);
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
	if (memoTrace.active) memoOp({ MemoOp::Layer, area, 0, 0, layerName, std::string() });
	outputKeys.clear();
	const auto layer = layers.layerMap.find(layerName);
	if (layer == layers.layerMap.end()) {
		throw out_of_range("ERROR: Layer(): a layer named as \"" + layerName + "\" doesn't exist.");
	}
	layerAt(layer->second, area);
}

// Layer(), with the layer's index
void OsmLuaProcessing::layerAt(unsigned layerIndex, bool area) {
	outputKeys.clear();
	uint layerMinZoom = layers.layers[layerIndex].minzoom;
	AttributeSet attributes;
	OutputGeometryType geomType = isRelation ? (area ? POLYGON_ : MULTILINESTRING_ ) :
	                                   isWay ? (area ? POLYGON_ : LINESTRING_) : POINT_;
	try {
		// Lua profiles often write the same geometry twice, e.g. a river and its name,
		// a highway and its name. Avoid duplicating geometry processing and storage
		// when this occurs.
		if (lastStoredGeometryId != 0 && lastStoredGeometryType == geomType) {
			OutputObject oo(geomType, layerIndex, lastStoredGeometryId, 0, layerMinZoom);
			outputs.push_back(std::make_pair(std::move(oo), attributes));
			return;
		}

		if (geomType==POINT_) {
			Point p = Point(lon, latp);

			if(CorrectGeometry(p) == CorrectGeometryResult::Invalid) return;

			NodeID id = USE_NODE_STORE | originalOsmID;
			if (materializeGeometries)
				id = osmMemTiles.storePoint(p);
			OutputObject oo(geomType, layerIndex, id, 0, layerMinZoom);
			outputs.push_back(std::make_pair(std::move(oo), attributes));
			noteIfDecluttered(LatpLon { latp, lon });
			return;
		}
		else if (geomType==POLYGON_) {
			// polygon

			if (isRelation) {
				try {
					// Checked in the cache, copied only when it needs correcting (as CorrectGeometry)
					const MultiPolygon &cached = multiPolygonCached();
					bool reversedValid = false;
					const geom::validity_failure_type failure = CheckGeometry(cached, &reversedValid);
					if (failure == geom::validity_failure_type::no_failure) {
						NodeID id = osmMemTiles.storeMultiPolygon(cached, true);
						OutputObject oo(geomType, layerIndex, id, 0, layerMinZoom);
						outputs.push_back(std::make_pair(std::move(oo), attributes));
						return;
					}
					MultiPolygon mp = cached;
					auto correctionResult = CorrectGeometryAfterCheck(mp, failure, false, reversedValid);
					if(correctionResult == CorrectGeometryResult::Invalid) return;
					// make_valid usually succeeds; checking that once here saves
					// re-validating the relation in every tile it is clipped to.
					// A big polygon is checked in the background until the tiles are written.
					const bool checked = correctionResult == CorrectGeometryResult::Valid || PrecheckValid(mp);
					const bool checkLater = !checked && geom::num_points(mp) >= 100000;
					const bool knownValid = checked || (!checkLater && IsValidIndexed(mp));
					NodeID id = osmMemTiles.storeMultiPolygon(mp, knownValid);
					if (checkLater) osmMemTiles.checkValidityLater(id, std::move(mp));
					OutputObject oo(geomType, layerIndex, id, 0, layerMinZoom);
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
				OutputObject oo(geomType, layerIndex, id, 0, layerMinZoom);
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
			OutputObject oo(geomType, layerIndex, id, 0, layerMinZoom);
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
				OutputObject oo(geomType, layerIndex, id, 0, layerMinZoom);
				outputs.push_back(std::make_pair(std::move(oo), attributes));
			} else {
				NodeID id = osmMemTiles.storeLinestring(ls);
				lastStoredGeometryId = id;
				lastStoredGeometryType = geomType;
				OutputObject oo(geomType, layerIndex, id, 0, layerMinZoom);
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
	if (layers.layerMap.count(layerName) == 0) {
		throw out_of_range("ERROR: LayerAsCentroid(): a layer named as \"" + layerName + "\" doesn't exist.");
	}
	// The first argument is the algorithm, the others relation roles to look for
	CentroidAlgorithm algorithm = defaultCentroidAlgorithm();
	std::vector<std::string> relationRoles;
	bool first = true;
	for (auto needleRef : varargs) {
		const std::string needle = needleRef.get<std::string>();
		if (first) algorithm = parseCentroidAlgorithm(needle);
		else relationRoles.push_back(needle);
		first = false;
	}
	if (memoTrace.active) {
		if (isRelation && !relationRoles.empty()) memoLuaOnly();
		else memoOp({ MemoOp::Centroid, false, 0, static_cast<double>(static_cast<int>(algorithm)), layerName, std::string() });
	}
	layerAsCentroid(layerName, algorithm, relationRoles);
}

void OsmLuaProcessing::layerAsCentroid(const std::string &layerName, CentroidAlgorithm algorithm, const std::vector<std::string> &relationRoles) {
	outputKeys.clear();
	layerAsCentroidAt(layers.layerMap[layerName], algorithm, relationRoles);
}

// layerAsCentroid(), with the layer's index
void OsmLuaProcessing::layerAsCentroidAt(unsigned layerIndex, CentroidAlgorithm algorithm, const std::vector<std::string> &relationRoles) {
	outputKeys.clear();

	// This will be non-zero if we ultimately used a node from a relation to
	// label the point.
	NodeID relationNode = 0;

	uint layerMinZoom = layers.layers[layerIndex].minzoom;
	AttributeSet attributes;
	Point geomp;
	bool centroidFound = false;
	try {
		// If we're a relation, see if the user would prefer we use one of its members
		// to label the point.
		if (isRelation) {
			for (const std::string &needle : relationRoles) {

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
	OutputObject oo(POINT_, layerIndex, id, 0, layerMinZoom);
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
	if (memoTrace.active) memoOp({ MemoOp::ModifyId, false, 0, static_cast<double>(newId), std::string(), std::string() });
    originalOsmID = static_cast<int64_t>(newId);
}

// Set attributes in a vector tile's Attributes table
void OsmLuaProcessing::Attribute(const string &key, const protozero::data_view val, const char minzoom) {
	if (memoTrace.active) memoStringAttribute({ MemoOp::Attr, false, minzoom, 0, key, std::string(val.data(), val.size()) });
	if (outputs.size()==0) { ProcessingError("Can't add Attribute if no Layer set"); return; }
	const uint16_t keyIndex = attributeStore.keyStore.key2index(key);
	removeAttributeIfNeeded(keyIndex);
	attributeStore.addAttributeWithKeyIndex(outputs.back().second, keyIndex, key, val, minzoom);
	setVectorLayerMetadata(outputs.back().first.layer, key, 0);
}

void OsmLuaProcessing::AttributeNumeric(const string &key, const double val, const char minzoom) {
	if (memoTrace.active) memoNumberAttribute({ MemoOp::AttrNumeric, false, minzoom, static_cast<double>(val), key, std::string() });
	if (outputs.size()==0) { ProcessingError("Can't add Attribute if no Layer set"); return; }
	const uint16_t keyIndex = attributeStore.keyStore.key2index(key);
	removeAttributeIfNeeded(keyIndex);
	attributeStore.addAttributeWithKeyIndex(outputs.back().second, keyIndex, val, minzoom);
	setVectorLayerMetadata(outputs.back().first.layer, key, 1);
}

void OsmLuaProcessing::AttributeBoolean(const string &key, const bool val, const char minzoom) {
	if (memoTrace.active) memoOp({ MemoOp::AttrBoolean, val, minzoom, 0, key, std::string() });
	if (outputs.size()==0) { ProcessingError("Can't add Attribute if no Layer set"); return; }
	const uint16_t keyIndex = attributeStore.keyStore.key2index(key);
	removeAttributeIfNeeded(keyIndex);
	attributeStore.addAttributeWithKeyIndex(outputs.back().second, keyIndex, val, minzoom);
	setVectorLayerMetadata(outputs.back().first.layer, key, 2);
}

void OsmLuaProcessing::AttributeInteger(const string &key, const int val, const char minzoom) {
	if (memoTrace.active) memoNumberAttribute({ MemoOp::AttrInteger, false, minzoom, static_cast<double>(val), key, std::string() });
	if (outputs.size()==0) { ProcessingError("Can't add Attribute if no Layer set"); return; }
	const uint16_t keyIndex = attributeStore.keyStore.key2index(key);
	removeAttributeIfNeeded(keyIndex);
	attributeStore.addAttributeWithKeyIndex(outputs.back().second, keyIndex, val, minzoom);
	setVectorLayerMetadata(outputs.back().first.layer, key, 3);
}

// Set minimum zoom
void OsmLuaProcessing::MinZoom(const double z) {
	if (memoTrace.active) memoOp({ MemoOp::MinZoom, false, 0, z, std::string(), std::string() });
	if (outputs.size()==0) { ProcessingError("Can't set minimum zoom if no Layer set"); return; }
	outputs.back().first.setMinZoom(z);
}

// Set the score used to rank this feature against its neighbours when decluttering
void OsmLuaProcessing::Score(const double score) {
	if (memoTrace.active) memoOp({ MemoOp::Score, false, 0, score, std::string(), std::string() });
	if (outputs.size()==0) { ProcessingError("Can't set score if no Layer set"); return; }
	if (declutterOutputs.empty() || declutterOutputs.back().outputIndex != outputs.size()-1) {
		ProcessingError("Score() only applies to point features in a layer with declutter_below set");
		return;
	}
	declutterOutputs.back().score = OutputObject::finite_cast<int32_t>(score);
}

// Set z_order
void OsmLuaProcessing::ZOrder(const double z) {
	if (memoTrace.active) memoOp({ MemoOp::ZOrder, false, 0, z, std::string(), std::string() });
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
	if (memoTrace.active) {
		MemoOp op{ static_cast<MemoOp::Kind>(MemoOp::FastStr + type), false, minzoom, 0, std::string(key, keyLen), std::string() };
		if constexpr (std::is_same<T, protozero::data_view>::value) op.str.assign(val.data(), val.size());
		else if constexpr (std::is_same<T, bool>::value) op.flag = val;
		else op.number = static_cast<double>(val);
		if constexpr (std::is_same<T, protozero::data_view>::value) memoStringAttribute(std::move(op));
		else if constexpr (std::is_same<T, bool>::value) memoOp(std::move(op));
		else memoNumberAttribute(std::move(op));
	}
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
		throw std::runtime_error("Lua error on scanning relation " + std::to_string(originalOsmID));
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
		RelationID id = relp.first;
		// only accepted relations have stored tags; leave the others untouched
		if (!osmStore.scannedRelations.has_relation_tags(id)) continue;
		reset();
		isPostScanRelation = true;
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
		if (nodeMemoEnabled) { memoSelect(true); runWayFunction(); }
		else if (!callLuaGlobal("node_function", true)) luaState["node_function"]();
	} catch(luaProcessingException &e) {
		throw std::runtime_error("Lua error on node " + std::to_string(originalOsmID));
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

	if (supportsReadingRelations) {
		if (const auto *relations = osmStore.scannedRelations.findWayRelations(wayId, wayRelationsCursor))
			relationList = *relations;
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
			memoSelect(false);
			runWayFunction();
		} catch(luaProcessingException &e) {
			throw std::runtime_error("Lua error on way " + std::to_string(originalOsmID));
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
			memoSelect(MemoRelations);
			runWayFunction();
		} else if (!isNativeMP && supportsWritingRelations) {
			if (!callLuaGlobal("relation_function", true)) luaState["relation_function"]();
		}
	} catch(luaProcessingException &e) {
		throw std::runtime_error("Lua error on relation " + std::to_string(originalOsmID));
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
		if (i < outputPreset.size() && outputPreset[i] != UINT32_MAX) {
			output.first.setAttributeSet(outputPreset[i]);
			attributeStore.noteLookup();
		} else {
			const AttributeIndex index = attributeStore.add(output.second);
			output.first.setAttributeSet(index);
			if (i < outputLearn.size() && outputLearn[i] != nullptr) {
				MemoOp &learn = *outputLearn[i];
				if (learn.segment & MemoOp::SegmentConstant) learn.segmentSet = index;
				else {
					char sig[MemoSigCache::SIG_MAX];
					const size_t len = memoSegmentSig(learn, sig);
					memoSegmentSets.put(MemoSigCache::hashOf(sig, len), sig, len, index);
				}
			}
		}
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

// ----	Memo of way_function (see osm_lua_processing.h)

// A node branching more often than this reads something that is different for most ways (a name,
// a house number): from there on the ways run through Lua. Values such as building heights or
// rare building types are many, but each comes often enough to pay for its path (planet: 256
// replays ~60% of the ways 64 left to Lua; more gains little). At most 65535.
static const size_t MEMO_MAX_CHILDREN = 256;
// per thread
static const size_t MEMO_MAX_NODES = 1 << 18;
// A full tree is compacted (memoCompact) after this many objects it had no room for
static const size_t MEMO_COMPACT_MISSES = std::max<size_t>(MEMO_MAX_NODES / 64, 16);

void OsmLuaProcessing::printMemoStats() {
	std::lock_guard<std::mutex> lock(memoInstancesMutex);
	uint64_t counts[MEMO_KINDS][4];
	for (int k = 0; k < MEMO_KINDS; k++) for (int i = 0; i < 4; i++) counts[k][i] = memoDone[k][i];
	for (auto *p : memoInstances) p->memoCounts(counts);
	const uint64_t *way = counts[MemoWays], *node = counts[MemoNodes], *rel = counts[MemoRelations];
	if (way[0] + way[1] + way[2] != 0)
		std::cout << "way_function memo: " << way[0] << " replayed, " << way[1] << " recorded, " << way[2] << " through Lua, " << way[3] << " nodes" << std::endl;
	if (node[0] + node[1] + node[2] != 0)
		std::cout << "node_function memo: " << node[0] << " replayed, " << node[1] << " recorded, " << node[2] << " through Lua, " << node[3] << " nodes" << std::endl;
	if (rel[0] + rel[1] + rel[2] != 0)
		std::cout << "way_function memo (multipolygons): " << rel[0] << " replayed, " << rel[1] << " recorded, " << rel[2] << " through Lua, " << rel[3] << " nodes" << std::endl;
}

void OsmLuaProcessing::runWayFunctionInLua(bool record) {
	if (record) {
		memoTrace.luaOnly = false;
		memoTrace.ambiguous = false;
		memoTrace.reads.clear();
		memoTrace.ops.clear();
		memoTrace.emptyValues.clear();
		memoTrace.values.clear();
	}
	memoTrace.active = record;
	if (memoForNodes) {
		// as setNode called it before the memo
		if (!callLuaGlobal("node_function", true)) luaState["node_function"]();
	} else if (!callLuaGlobal("way_function", false)) {
		kaguya::LuaFunction way_function = luaState["way_function"];
		kaguya::LuaRef ret = way_function();
		assert(!ret);
	}
	memoTrace.active = false;
	if (record) memoInsert();
}

// The walk reads the way's tags through memoSlots (filled once per way from the tags as added,
// see memo_keys.h), which answers as currentTags->getKey/getValueFromKey would.
void OsmLuaProcessing::runWayFunction() {
	if (!memoEnabled) { runWayFunctionInLua(false); return; }
	if (memoNodes.empty()) memoNodes.emplace_back();
	const bool canRecord = memoNodes.size() < MEMO_MAX_NODES;
	for (size_t id = memoKeys.valueOnly.size(); id < memoKeys.size(); id++)
		memoKeys.valueOnly.push_back(memoValueTag(memoKeys.name(id).data(), memoKeys.name(id).size()));
	// IsClosed() as Lua sees it: false for a node
	const bool closed = IsClosed(), hasRelations = !relationList.empty();
	char sig[MemoSigCache::SIG_MAX];
	const size_t sigLen = memoSlots.fill(memoKeys, currentTags->added(), uint8_t(closed) | uint8_t(hasRelations) << 1, sig, sizeof(sig));
	const uint64_t sigHash = sigLen ? MemoSigCache::hashOf(sig, sigLen) : 0;
	uint32_t last = 0;
	uint32_t n = sigLen ? memoWalkCache.find(sigHash, sig, sigLen) : UINT32_MAX;
	if (n == UINT32_MAX) {
		// The reads that aren't tags, as rawNextRelation, rawMercatorArea, rawFindInRelation record
		// them. A walk that made one isn't a function of the signature: it isn't cached.
		struct Extra {
			OsmLuaProcessing &p;
			bool uncached = false;
			int relation = -1;	// as relationSubscript
			std::string result;
			std::string_view nextRelation() {
				if (!p.memoRelations) return "1";	// such a way runs through Lua: no reads after this one
				uncached = true;
				if (size_t(++relation) >= p.relationList.size()) return "0";
				result = "1" + p.osmStore.scannedRelations.getRole(p.relationList[relation].second);
				return result;
			}
			std::string_view area() {
				uncached = true;
				result = p.memoAreaResult(p.MercatorArea());
				return result;
			}
			std::string_view findInRelation(uint32_t key) {
				uncached = true;
				if (relation < 0 || size_t(relation) >= p.relationList.size()) return "\xff";	// not recorded: no child
				result = p.osmStore.scannedRelations.get_relation_tag(p.relationList[relation].first, p.memoKeys.name(key));
				return result;
			}
		} extra{ *this };
		n = memoWalk(memoNodes.data(), memoSlots, closed, hasRelations, [this](uint32_t k, std::string_view r) {
			const MemoHotNode &node = memoNodes[k];
			return node.more == UINT32_MAX ? UINT32_MAX : memoMore[node.more].child(r);
		}, extra, last);
		if (sigLen && !extra.uncached && n != UINT32_MAX && (memoNodes[n].kind == MemoNode::Leaf || memoNodes[n].kind == MemoNode::Lua))
			memoWalkCache.put(sigHash, sig, sigLen, n);
	}
	if (n == UINT32_MAX && memoNodes[last].overflow) { memoLua++; runWayFunctionInLua(false); return; }
	if (n != UINT32_MAX && memoNodes[n].kind == MemoNode::Leaf) {
		MemoMore &leaf = memoMore[memoNodes[n].more];
		if (leaf.uses != UINT32_MAX) leaf.uses++;
		memoHits++; memoReplay(leaf.ops); return;
	}
	if (n != UINT32_MAX && memoNodes[n].kind == MemoNode::Lua) {
		MemoMore &end = memoMoreOf(n);
		if (end.uses != UINT32_MAX) end.uses++;
		memoLua++; runWayFunctionInLua(false); return;
	}
	// no child for the result read, or Unset: record, if the tree has room
	memoRecorded++;
	if (!canRecord && ++memoFullMisses >= MEMO_COMPACT_MISSES) {
		memoCompact();
		runWayFunctionInLua(memoNodes.size() < MEMO_MAX_NODES);
		return;
	}
	runWayFunctionInLua(canRecord);
}

// The tree is full, and objects keep taking paths it hasn't got: keep only the paths that objects
// have taken since the last time (Leaf and Lua nodes used, and the reads to them), so that the
// paths of the objects to come can be recorded. The tree then is as if those paths had been
// recorded alone (in the same order).
void OsmLuaProcessing::memoCompact() {
	const size_t size = memoNodes.size();
	std::vector<uint32_t> remap(size, UINT32_MAX);
	auto keptChild = [&](uint32_t c) { return c != UINT32_MAX && remap[c] != UINT32_MAX; };
	// A child has a higher index than its parent: going backwards, the children are decided
	std::vector<uint8_t> keep(size, 0);
	for (size_t i = size; i-- > 0; ) {
		const MemoHotNode &node = memoNodes[i];
		bool k = i == 0;
		if (node.kind == MemoNode::Leaf || node.kind == MemoNode::Lua) k = k || (node.more != UINT32_MAX && memoMore[node.more].uses > 0);
		else if (node.kind != MemoNode::Unset) {
			for (int q = 0; q < 4 && !k; q++) k = node.quick[q] != UINT32_MAX && keep[node.quick[q]];
			if (!k && node.more != UINT32_MAX)
				for (const auto &c : memoMore[node.more].children) if (keep[c.second]) { k = true; break; }
		}
		keep[i] = k;
	}
	uint32_t next = 0;
	for (size_t i = 0; i < size; i++) if (keep[i]) remap[i] = next++;
	std::vector<MemoHotNode> nodes;
	std::vector<MemoMore> more;
	nodes.reserve(next);
	for (size_t i = 0; i < size; i++) {
		if (!keep[i]) continue;
		const MemoHotNode &old = memoNodes[i];
		MemoHotNode node;
		node.kind = old.kind; node.valueOnly = old.valueOnly; node.overflow = old.overflow; node.key = old.key;
		node.children = 0;
		for (int q = 0; q < 4; q++)
			if (keptChild(old.quick[q])) { node.quick[q] = remap[old.quick[q]]; node.children++; }
		if (old.more != UINT32_MAX) {
			MemoMore &om = memoMore[old.more];
			MemoMore m;
			for (const auto &c : om.children)
				if (keptChild(c.second)) { m.addChild(c.first, remap[c.second]); node.children++; }
			m.ops = std::move(om.ops);
			if (!m.children.empty() || !m.ops.empty() || node.kind == MemoNode::Leaf || node.kind == MemoNode::Lua) {
				node.more = more.size();
				more.push_back(std::move(m));
			}
		}
		// An overflowed node that lost children takes new results again (the results used often
		// stay: their paths were used, so kept)
		if (node.children < MEMO_MAX_CHILDREN) node.overflow = false;
		nodes.push_back(node);
	}
	memoNodes.swap(nodes);
	memoMore.swap(more);
	memoWalkCache.clear();
	memoComputeJumps(memoNodes.data(), memoNodes.size());
	memoJumpsAt = memoNodes.size();
	memoFullMisses = 0;
}

// Add the trace just recorded to the tree
void OsmLuaProcessing::memoInsert() {
	if (memoTrace.ambiguous && !memoTrace.luaOnly) return;
	auto nondeterministic = [&]() {
		memoEnabled = false;
		if (memoForNodes) std::cerr << "node_memo: node_function read differently for the same input (node " << originalOsmID << "); memo switched off for this thread" << std::endl;
		else if (memoKind == MemoRelations) std::cerr << "way_memo: way_function read differently for the same input (relation " << originalOsmID << "); memo switched off for relations on this thread" << std::endl;
		else std::cerr << "way_memo: way_function read differently for the same input (way " << originalOsmID << "); memo switched off for this thread" << std::endl;
	};
	uint32_t n = 0;
	const size_t sizeBefore = memoNodes.size();
	for (const MemoRead &r : memoTrace.reads) {
		// key ids: equal ids <=> equal keys (IsClosed, NextRelation, MercatorArea: no key)
		const uint32_t key = r.kind == MemoNode::Find || r.kind == MemoNode::Holds || r.kind == MemoNode::FindInRelation ? memoKeys.add(r.key) : MemoKeyTable::NONE;
		if (memoNodes[n].kind == MemoNode::Unset) { memoNodes[n].kind = r.kind; memoNodes[n].key = key; memoNodes[n].valueOnly = r.valueOnly; }
		else if (memoNodes[n].kind == MemoNode::Lua) return;
		else if (memoNodes[n].kind != r.kind || memoNodes[n].key != key || memoNodes[n].valueOnly != r.valueOnly) { nondeterministic(); return; }
		uint32_t next = memoChild(n, r.result);
		if (next == UINT32_MAX) {
			if (memoNodes[n].children >= MEMO_MAX_CHILDREN) {
				memoNodes[n].overflow = true;
				return;
			}
			next = memoNodes.size();
			memoNodes.emplace_back();
			memoAddChild(n, r.result, next);
		}
		n = next;
	}
	MemoHotNode &end = memoNodes[n];
	if (end.kind == MemoNode::Unset) {
		if (memoTrace.luaOnly) end.kind = MemoNode::Lua;
		else {
			end.kind = MemoNode::Leaf;
			std::vector<MemoOp> &ops = memoMoreOf(n).ops;
			ops = memoTrace.ops;
			for (MemoOp &op : ops) if (!op.valueKey.empty()) op.valueKeyId = memoKeys.add(op.valueKey);
		}
	} else if (end.kind != MemoNode::Lua && (memoTrace.luaOnly || end.kind != MemoNode::Leaf)) {
		nondeterministic();
	}
	// The jumps of the nodes just added (their children are newer: none yet), and those of all
	// nodes when the tree has doubled since the last time (geometrically: O(nodes) in all) and once
	// more when it is full, so that older nodes' jumps reach into the paths added since
	if (memoNodes.size() >= memoJumpsAt * 2 + 64 || (memoNodes.size() >= MEMO_MAX_NODES && memoJumpsAt < memoNodes.size())) {
		memoComputeJumps(memoNodes.data(), memoNodes.size());
		memoJumpsAt = memoNodes.size();
	} else if (memoNodes.size() > sizeBefore) memoComputeJumps(memoNodes.data(), memoNodes.size(), sizeBefore);
}

// Make the calls way_function made
//
// The calls of one leaf are the same every time, but for the values of way_memo_values tags
// (valueKey) and whether a Layer/Centroid call adds an output (that depends on the geometry). So the
// layer index, key index and pair of an attribute op without a valueKey are worked out once and
// kept in the op (the pair index for the same pair never changes). An output gets the attribute
// ops after its layer op up to the next layer op (its segment), and those of following layer ops
// that add no output. If its segment has no valueKey ops, and it gets no others, its attribute
// set is always the same, and so its AttributeIndex (one per distinct set): the first time it is
// kept in the layer op (segmentSet), and later replays skip the segment's attribute ops and the
// set lookup. Should a later layer op add no output but have attribute ops, the skipped ops are
// made after all (as they were, with outputKeys as Layer left it), and then the later ones.
static inline bool memoIsLayerOp(OsmLuaProcessing::MemoOp::Kind k) { return k == OsmLuaProcessing::MemoOp::Layer || k == OsmLuaProcessing::MemoOp::Centroid; }
static inline bool memoIsAttributeOp(OsmLuaProcessing::MemoOp::Kind k) { return k >= OsmLuaProcessing::MemoOp::Attr && k <= OsmLuaProcessing::MemoOp::FastInteger; }

void OsmLuaProcessing::memoReplay(std::vector<MemoOp> &ops) {
	bool skipping = false;	// the attribute ops are those of outputs.back()'s segment, whose AttributeIndex is known
	for (size_t i = 0; i < ops.size(); i++) {
		MemoOp &op = ops[i];
		if (memoIsLayerOp(op.kind)) {
			if (op.layerIndex < 0) {
				const auto layer = layers.layerMap.find(op.key);
				if (layer == layers.layerMap.end()) { Layer(op.key, op.flag); continue; }	// throws
				op.layerIndex = layer->second;
			}
			if (!(op.segment & MemoOp::SegmentKnown)) {
				uint8_t s = MemoOp::SegmentKnown | MemoOp::SegmentConstant;
				size_t j = i + 1;
				for (; j < ops.size() && !memoIsLayerOp(ops[j].kind); j++)
					if (memoIsAttributeOp(ops[j].kind)) {
						s |= MemoOp::SegmentAttributes;
						if (!ops[j].valueKey.empty()) s &= ~MemoOp::SegmentConstant;
					}
				op.segment = s;
				op.segmentOps = j - i - 1;
				op.segmentId = ++memoSegmentIds;
			}
			const size_t before = outputs.size();
			if (op.kind == MemoOp::Layer) layerAt(op.layerIndex, op.flag);
			else layerAsCentroidAt(op.layerIndex, static_cast<CentroidAlgorithm>(static_cast<int>(op.number)), {});
			if (outputs.size() > before) {
				outputPreset.resize(before, UINT32_MAX); outputLearn.resize(before, nullptr); outputSegment.resize(before, UINT32_MAX);
				const bool constant = op.segment & MemoOp::SegmentConstant;
				AttributeIndex preset = constant ? op.segmentSet : UINT32_MAX;
				bool learn = constant && preset == UINT32_MAX;
				if (!constant && (op.segment & MemoOp::SegmentAttributes) && op.segmentId != 0) {
					// its set is a function of the segment and the values of its way_memo_values tags
					char sig[MemoSigCache::SIG_MAX];
					const size_t len = memoSegmentSig(op, sig);
					if (len) {
						preset = memoSegmentSets.find(MemoSigCache::hashOf(sig, len), sig, len);
						learn = preset == UINT32_MAX;
					}
				}
				skipping = preset != UINT32_MAX;
				outputPreset.push_back(preset);
				outputLearn.push_back(learn ? &op : nullptr);
				outputSegment.push_back(i);
			} else {
				// This segment's attribute ops go to the output before, if any
				if ((op.segment & MemoOp::SegmentAttributes) && !outputs.empty() && outputPreset.size() == outputs.size()) {
					if (outputPreset.back() != UINT32_MAX) memoSegmentAttributes(ops, outputSegment.back());
					outputPreset.back() = UINT32_MAX;
					outputLearn.back() = nullptr;
				}
				skipping = false;
			}
			continue;
		}
		if (memoIsAttributeOp(op.kind)) {
			if (skipping) continue;
			memoAttribute(op, memoOpValue(op));
			continue;
		}
		switch (op.kind) {
			case MemoOp::MinZoom: MinZoom(op.number); break;
			case MemoOp::ZOrder: ZOrder(op.number); break;
			case MemoOp::Score: Score(op.number); break;
			case MemoOp::ModifyId: ModifyId(static_cast<int>(op.number)); break;
			case MemoOp::MinZoomByArea: MinZoomByArea(op.number, op.numbers); break;
			default: break;
		}
	}
}

// The skipped attribute ops of the segment of layer op `layerOp`, onto outputs.back(), as made then
// (Layer had cleared outputKeys), and outputKeys as the layer op since has left it
void OsmLuaProcessing::memoSegmentAttributes(std::vector<MemoOp> &ops, size_t layerOp) {
	outputKeys.clear();
	for (size_t j = layerOp + 1; j < ops.size() && !memoIsLayerOp(ops[j].kind); j++)
		if (memoIsAttributeOp(ops[j].kind)) memoAttribute(ops[j], memoOpValue(ops[j]));
	outputKeys.clear();
}

// The value of an attribute op: its string, or for a valueKey op the current object's value of the
// tag, as currentTags->getKey/getValueFromKey (the leaf's value keys are in memoKeys since before
// the walk). It is copied if the attribute store keeps it.
protozero::data_view OsmLuaProcessing::memoOpValue(const MemoOp &op) const {
	if (op.valueKey.empty()) return protozero::data_view(op.str.data(), op.str.size());
	const std::string_view v = memoSlots.get(op.valueKeyId);
	return v.empty() ? protozero::data_view("", 0) : protozero::data_view(v.data(), v.size());
}

// The signature of the attribute set of an output of layer op `layerOp`, for a segment with valueKey
// ops: the segment's id, then the value of each of its valueKey ops (its length, its bytes). The
// other attribute ops are the same each time, so equal signatures mean equal sets. 0: too long.
size_t OsmLuaProcessing::memoSegmentSig(const MemoOp &layerOp, char *sig) const {
	size_t len = 0;
	memcpy(sig, &layerOp.segmentId, sizeof(layerOp.segmentId)); len += sizeof(layerOp.segmentId);
	const MemoOp *ops = &layerOp + 1;
	for (uint32_t k = 0; k < layerOp.segmentOps; k++) {
		const MemoOp &op = ops[k];
		if (!memoIsAttributeOp(op.kind) || op.valueKey.empty()) continue;
		const std::string_view v = memoSlots.get(op.valueKeyId);
		if (v.size() >= 256 || len + 1 + v.size() > MemoSigCache::SIG_MAX) return 0;
		sig[len++] = char(v.size());
		memcpy(sig + len, v.data(), v.size());
		len += v.size();
	}
	return len;
}

// An attribute op of the replay: as Attribute*()/attributeFast(), with the key index, the pair
// (no valueKey) and the vector_layers bookkeeping kept in the op
void OsmLuaProcessing::memoAttribute(MemoOp &op, const protozero::data_view &str) {
	if (outputs.size()==0) { ProcessingError("Can't add Attribute if no Layer set"); return; }
	if (op.keyIndex < 0) op.keyIndex = attributeStore.keyStore.key2index(op.key);
	removeAttributeIfNeeded(op.keyIndex);
	AttributeSet &set = outputs.back().second;
	uint type = 0;
	switch (op.kind) {
		case MemoOp::Attr: case MemoOp::FastStr:
			type = 0;
			if (!op.valueKey.empty()) { attributeStore.addAttributeWithKeyIndex(set, op.keyIndex, op.key, str, op.minzoom); break; }
			if (op.pairIndex == 0) op.pairIndex = attributeStore.pairIndexWithKeyIndex(op.keyIndex, op.key, str, op.minzoom);
			set.addPair(op.pairIndex);
			break;
		case MemoOp::AttrNumeric: case MemoOp::FastNumeric:
			type = 1;
			if (op.valueLength) { attributeStore.addAttributeWithKeyIndex(set, op.keyIndex, static_cast<double>(str.size()), op.minzoom); break; }
			if (op.pairIndex == 0) op.pairIndex = attributeStore.pairIndexWithKeyIndex(op.keyIndex, op.number, op.minzoom);
			set.addPair(op.pairIndex);
			break;
		case MemoOp::AttrBoolean: case MemoOp::FastBoolean:
			type = 2;
			if (op.pairIndex == 0) op.pairIndex = attributeStore.pairIndexWithKeyIndex(op.keyIndex, op.flag, op.minzoom);
			set.addPair(op.pairIndex);
			break;
		default:	// AttrInteger, FastInteger
			type = 3;
			if (op.valueLength) { attributeStore.addAttributeWithKeyIndex(set, op.keyIndex, static_cast<int>(str.size()), op.minzoom); break; }
			if (op.pairIndex == 0) op.pairIndex = attributeStore.pairIndexWithKeyIndex(op.keyIndex, static_cast<int>(op.number), op.minzoom);
			set.addPair(op.pairIndex);
			break;
	}
	const uint_least8_t layer = outputs.back().first.layer;
	if (op.seenLayer != layer) {
		setVectorLayerMetadata(layer, op.key, type);
		op.seenLayer = layer;
	}
}

void OsmLuaProcessing::memoStringAttribute(MemoOp &&op) {
	if (!memoTrace.values.empty() && !op.str.empty()) {
		// Which way_memo_values tags has this value? Exactly one, and this attribute one of its: a placeholder
		const std::pair<std::string, std::string> *source = nullptr;
		int matches = 0;
		for (const auto &v : memoTrace.values) if (v.second == op.str) { source = &v; matches++; }
		if (matches > 0) {
			const auto it = memoValueTags.find(source->first);
			const bool declared = matches == 1 && std::find(it->second.begin(), it->second.end(), op.key) != it->second.end();
			if (!declared) { memoTrace.ambiguous = true; return; }
			op.valueKey = source->first;
			op.str.clear();
			memoOp(std::move(op));
			return;
		}
	}
	// An empty declared attribute: a copy of a tag it is declared for, read empty (as for every
	// object that replays this, so the constant is right)
	if (op.str.empty())
		for (const auto &e : memoTrace.emptyValues) {
			const auto it = memoValueTags.find(e);
			if (it != memoValueTags.end() && std::find(it->second.begin(), it->second.end(), op.key) != it->second.end()) { memoOp(std::move(op)); return; }
		}
	// A declared attribute that isn't a plain copy: the profile does more with the value than declared
	for (const auto &t : memoValueTags)
		if (std::find(t.second.begin(), t.second.end(), op.key) != t.second.end()) { memoLuaOnly(); return; }
	memoOp(std::move(op));
}

// { tag = { "attribute", ..., attribute = "length", ... }, ... }: the attributes the tag's value is
// copied into, and (lengths: attribute -> tag) those that are set to its length
void OsmLuaProcessing::memoNumberAttribute(MemoOp &&op) {
	if (!memoValueLengths.empty()) {
		const auto it = memoValueLengths.find(op.key);
		if (it != memoValueLengths.end()) {
			// declared as the length of a tag: a placeholder if it is that (else the profile does more than declared)
			for (const auto &v : memoTrace.values)
				if (v.first == it->second && static_cast<double>(v.second.size()) == op.number) {
					op.valueKey = v.first;
					op.valueLength = true;
					op.number = 0;
					memoOp(std::move(op));
					return;
				}
			// the length of the tag read empty (as for every object that replays this): a constant
			if (op.number == 0 && std::find(memoTrace.emptyValues.begin(), memoTrace.emptyValues.end(), it->second) != memoTrace.emptyValues.end()) {
				memoOp(std::move(op));
				return;
			}
			memoLuaOnly();
			return;
		}
	}
	memoOp(std::move(op));
}

// The comparisons of the area with the memo_area numbers: per number '0' (<), '1' (>), '2' (==), '3' (NaN)
std::string OsmLuaProcessing::memoAreaResult(double area) const {
	std::string r(memoArea.size(), '3');
	for (size_t i = 0; i < memoArea.size(); i++)
		r[i] = area < memoArea[i] ? '0' : area > memoArea[i] ? '1' : area == memoArea[i] ? '2' : '3';
	return r;
}

// The length of the table at idx (the declarations are plain tables: no __len). luaL_len is Lua 5.2+;
// Lua 5.1 and LuaJIT have lua_objlen.
static lua_Integer memoTableLength(lua_State *L, int idx) {
#if LUA_VERSION_NUM >= 502
	return luaL_len(L, idx);
#else
	return static_cast<lua_Integer>(lua_objlen(L, idx));
#endif
}

static std::unordered_map<std::string, std::vector<std::string>> readMemoValueTags(lua_State *L, const char *global, std::unordered_map<std::string, std::string> &lengths) {
	std::unordered_map<std::string, std::vector<std::string>> tags;
	lua_getglobal(L, global);
	if (lua_istable(L, -1)) {
		lua_pushnil(L);
		while (lua_next(L, -2) != 0) {
			if (lua_type(L, -2) == LUA_TSTRING && lua_istable(L, -1)) {
				const std::string tag = lua_tostring(L, -2);
				std::vector<std::string> &attrs = tags[tag];
				const lua_Integer n = memoTableLength(L, -1);
				for (lua_Integer i = 1; i <= n; i++) {
					lua_rawgeti(L, -1, i);
					if (lua_type(L, -1) == LUA_TSTRING) attrs.push_back(lua_tostring(L, -1));
					lua_pop(L, 1);
				}
				lua_pushnil(L);
				while (lua_next(L, -2) != 0) {
					if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TSTRING && std::string(lua_tostring(L, -1)) == "length") {
						lua_pushvalue(L, -2);	// lua_tostring on a copy: lua_next needs the key unchanged
						lengths[lua_tostring(L, -1)] = tag;
						lua_pop(L, 1);
					}
					lua_pop(L, 1);
				}
			}
			lua_pop(L, 1);
		}
	}
	lua_pop(L, 1);
	return tags;
}

// { number, ... }: the numbers way_function compares MercatorArea() with. An integer that a double
// can't hold exactly would compare differently in Lua: then none (MercatorArea() runs through Lua).
static std::vector<double> readMemoArea(lua_State *L, const char *global) {
	std::vector<double> area;
	lua_getglobal(L, global);
	if (lua_istable(L, -1)) {
		const lua_Integer n = memoTableLength(L, -1);
		for (lua_Integer i = 1; i <= n; i++) {
			lua_rawgeti(L, -1, i);
			bool ok = lua_type(L, -1) == LUA_TNUMBER;
#if LUA_VERSION_NUM >= 503
			if (ok && lua_isinteger(L, -1)) {
				const lua_Integer v = lua_tointeger(L, -1);
				ok = v >= -(lua_Integer(1) << 53) && v <= (lua_Integer(1) << 53);
			}
#endif
			if (ok) area.push_back(lua_tonumber(L, -1));
			else { std::cerr << global << ": entry " << i << " isn't a number a double holds exactly; ignored (MercatorArea() runs through Lua)" << std::endl; area.clear(); lua_pop(L, 1); break; }
			lua_pop(L, 1);
		}
	}
	lua_pop(L, 1);
	return area;
}
