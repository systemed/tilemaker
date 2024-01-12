/*! \file */ 
#ifndef _OSM_LUA_PROCESSING_H
#define _OSM_LUA_PROCESSING_H

#include <vector>
#include <string>
#include <sstream>
#include <map>
#include "geom.h"
#include "osm_store.h"
#include "shared_data.h"
#include "output_object.h"
#include "shp_mem_tiles.h"
#include "osm_mem_tiles.h"
#include "helpers.h"
#include "pbf_reader.h"
#include <protozero/data_view.hpp>

#include <boost/container/flat_map.hpp>

class TagMap;
class SignificantTags;

// Lua
extern "C" {
	#include "lua.h"
    #include "lualib.h"
    #include "lauxlib.h"
}

#include "external/kaguya.hpp"

// FIXME: why is this global ?
extern bool verbose;

class AttributeStore;
class AttributeSet;

// A string, which might be in `currentTags` as a value. If Lua
// code refers to an absent value, it'll fallback to passing
// it as a std::string.
//
// The intent is that Attribute("name", Find("name")) is a common
// pattern, and we ought to avoid marshalling a string back and
// forth from C++ to Lua when possible.
struct PossiblyKnownTagValue {
	bool found;
	uint32_t index;
	std::string fallback;
};

/**
	\brief OsmLuaProcessing - converts OSM objects into OutputObjects.
	
	The input objects are generated by PbfReader. The output objects are sent to OsmMemTiles for storage.

	This class provides a consistent interface for Lua scripts to access.
*/
class OsmLuaProcessing { 

public:
	// ----	initialization routines

	OsmLuaProcessing(
		OSMStore &osmStore,
		const class Config &configIn,
		class LayerDefinition &layers, 
		const std::string &luaFile,
		const class ShpMemTiles &shpMemTiles, 
		class OsmMemTiles &osmMemTiles,
		AttributeStore &attributeStore,
		bool materializeGeometries
	);
	~OsmLuaProcessing();

	// ----	Helpers provided for main routine
	void handleUserSignal(int signum);

	// Has this object been assigned to any layers?
	bool empty();
	
	// Do we have Lua routines for non-MP relations?
	bool canReadRelations();
	bool canPostScanRelations();
	bool canWriteRelations();

	// Shapefile tag remapping
	bool canRemapShapefiles();
	kaguya::LuaTable newTable();
	kaguya::LuaTable remapAttributes(kaguya::LuaTable& in_table, const std::string &layerName);

	// ----	Data loading methods

	using tag_map_t = boost::container::flat_map<protozero::data_view, protozero::data_view, DataViewLessThan>;

	// Scan non-MP relation
	bool scanRelation(WayID id, const TagMap& tags);

	// Post-scan non-MP relations
	void postScanRelations();
	
	/// \brief We are now processing a significant node
	bool setNode(NodeID id, LatpLon node, const TagMap& tags);

	/// \brief We are now processing a way
	bool setWay(WayID wayId, LatpLonVec const &llVec, const TagMap& tags);

	/** \brief We are now processing a relation
	 * (note that we store relations as ways with artificial IDs, and that
	 *  we use decrementing positive IDs to give a bit more space for way IDs)
	 */
	void setRelation(
		const std::vector<protozero::data_view>& stringTable,
		const PbfReader::Relation& relation,
		const WayVec& outerWayVec,
		const WayVec& innerWayVec,
		const TagMap& tags,
		bool isNativeMP,
		bool isInnerOuter
	);

	// ----	Metadata queries called from Lua

	// Get the ID of the current object
	std::string Id() const;

	// Check if there's a value for a given key
	bool Holds(const std::string& key) const;

	// Get an OSM tag for a given key (or return empty string if none)
	const std::string Find(const std::string& key) const;

	// Check if an object has any tags
	bool HasTags() const;

	// ----	Spatial queries called from Lua

	// Find intersecting shapefile layer
	std::vector<std::string> FindIntersecting(const std::string &layerName);
	double AreaIntersecting(const std::string &layerName);
	bool Intersects(const std::string &layerName);
	template <typename GeometryT> double intersectsArea(const std::string &layerName, GeometryT &geom) const;
	template <typename GeometryT> std::vector<uint> intersectsQuery(const std::string &layerName, bool once, GeometryT &geom) const;

	std::vector<std::string> FindCovering(const std::string &layerName);
	bool CoveredBy(const std::string &layerName);
	template <typename GeometryT> std::vector<uint> coveredQuery(const std::string &layerName, bool once, GeometryT &geom) const;
		
	// Returns whether it is closed polygon
	bool IsClosed() const;

	// Returns area
	double Area();
	double multiPolygonArea(const MultiPolygon &mp) const;

	// Returns length
	double Length();
	
	// Return centroid lat/lon
	std::vector<double> Centroid(kaguya::VariadicArgType algorithm);

	enum class CentroidAlgorithm: char { Centroid = 0, Polylabel = 1 };
	CentroidAlgorithm defaultCentroidAlgorithm() const { return CentroidAlgorithm::Polylabel; }
	CentroidAlgorithm parseCentroidAlgorithm(const std::string& algorithm) const;
	Point calculateCentroid(CentroidAlgorithm algorithm);

	enum class CorrectGeometryResult: char { Invalid = 0, Valid = 1, Corrected = 2 };
	// ----	Requests from Lua to write this way/node to a vector tile's Layer
	template<class GeometryT>
	CorrectGeometryResult CorrectGeometry(GeometryT &geom)
	{
		geom::validity_failure_type failure = geom::validity_failure_type::no_failure;
		if (isRelation && !geom::is_valid(geom,failure)) {
			if (verbose) std::cout << "Relation " << originalOsmID << " has " << boost_validity_error(failure) << std::endl;
		} else if (isWay && !geom::is_valid(geom,failure)) {
			if (verbose && failure!=22) std::cout << "Way " << originalOsmID << " has " << boost_validity_error(failure) << std::endl;
		}
		
		if (failure==boost::geometry::failure_spikes)
			geom::remove_spikes(geom);
		if (failure == boost::geometry::failure_few_points) 
			return CorrectGeometryResult::Invalid;
		if (failure) {
			std::time_t start = std::time(0);
			make_valid(geom);
			if (verbose && std::time(0)-start>3) {
				std::cout << (isRelation ? "Relation " : "Way ") << originalOsmID << " took " << (std::time(0)-start) << " seconds to correct" << std::endl;
			}
			return CorrectGeometryResult::Corrected;
		}
		return CorrectGeometryResult::Valid;
	}

	// Add layer
	void Layer(const std::string &layerName, bool area);
	void LayerAsCentroid(const std::string &layerName, kaguya::VariadicArgType nodeSources);
	
	// Set attributes in a vector tile's Attributes table
	void Attribute(const std::string &key, const std::string &val);
	void AttributeWithMinZoom(const std::string &key, const std::string &val, const char minzoom);
	void AttributeNumeric(const std::string &key, const float val);
	void AttributeNumericWithMinZoom(const std::string &key, const float val, const char minzoom);
	void AttributeBoolean(const std::string &key, const bool val);
	void AttributeBooleanWithMinZoom(const std::string &key, const bool val, const char minzoom);
	void MinZoom(const double z);
	void ZOrder(const double z);
	
	// Relation scan support

	struct OptionalRelation {
		bool done;
		lua_Integer id;
		std::string role;
	};
	OptionalRelation NextRelation();
	void RestartRelations();
	std::string FindInRelation(const std::string &key);
	void Accept();
	void SetTag(const std::string &key, const std::string &value);

	// Write error if in verbose mode
	void ProcessingError(const std::string &errStr) {
		if (verbose) { std::cerr << errStr << std::endl; }
	}

	// ----	vector_layers metadata entry

	void setVectorLayerMetadata(const uint_least8_t layer, const std::string &key, const uint type);

	SignificantTags GetSignificantNodeKeys();
	SignificantTags GetSignificantWayKeys();

	// ---- Cached geometries creation

	const Linestring &linestringCached();

	const Polygon &polygonCached();

	const MultiLinestring &multiLinestringCached();

	const MultiPolygon &multiPolygonCached();

	inline AttributeStore &getAttributeStore() { return attributeStore; }

	struct luaProcessingException :std::exception {};
	const TagMap* currentTags;

private:
	/// Internal: clear current cached state
	inline void reset() {
		outputs.clear();
		currentRelation = nullptr;
		stringTable = nullptr;
		llVecPtr = nullptr;
		outerWayVecPtr = nullptr;
		innerWayVecPtr = nullptr;
		linestringInited = false;
		multiLinestringInited = false;
		polygonInited = false;
		multiPolygonInited = false;
		relationAccepted = false;
		relationList.clear();
		relationSubscript = -1;
		lastStoredGeometryId = 0;
		isWay = false;
		isRelation = false;
		isPostScanRelation = false;
	}

	void removeAttributeIfNeeded(const std::string& key);

	const inline Point getPoint() {
		return Point(lon/10000000.0,latp/10000000.0);
	}
	
	OSMStore &osmStore;	// global OSM store

	kaguya::State luaState;
	bool supportsRemappingShapefiles;
	bool supportsReadingRelations;
	bool supportsPostScanRelations;
	bool supportsWritingRelations;
	const class ShpMemTiles &shpMemTiles;
	class OsmMemTiles &osmMemTiles;
	AttributeStore &attributeStore;			// key/value store

	int64_t originalOsmID;					///< Original OSM object ID
	bool isWay, isRelation, isClosed;		///< Way, node, relation?

	bool relationAccepted;					// in scanRelation, whether we're using a non-MP relation
	std::vector<std::pair<WayID, uint16_t>> relationList;		// in processNode/processWay, list of relations this entity is in, and its role
	int relationSubscript = -1;				// in processWay, position in the relation list
	bool isPostScanRelation;				// processing a relation in postScanRelation

	int32_t lon,latp;						///< Node coordinates
	LatpLonVec const *llVecPtr;
	WayVec const *outerWayVecPtr;
	WayVec const *innerWayVecPtr;

	Linestring linestringCache;
	bool linestringInited;
	Polygon polygonCache;
	bool polygonInited;
	MultiLinestring multiLinestringCache;
	bool multiLinestringInited;
	MultiPolygon multiPolygonCache;
	bool multiPolygonInited;

	NodeID lastStoredGeometryId;
	OutputGeometryType lastStoredGeometryType;

	const class Config &config;
	class LayerDefinition &layers;

	std::vector<std::pair<OutputObject, AttributeSet>> outputs;		// All output objects that have been created
	std::vector<std::string> outputKeys;
	const PbfReader::Relation* currentRelation;
	const boost::container::flat_map<std::string, std::string>* currentPostScanTags; // for postScan only
	const std::vector<protozero::data_view>* stringTable;

	std::vector<OutputObject> finalizeOutputs();

	bool materializeGeometries;
	bool wayEmitted;
};

#endif //_OSM_LUA_PROCESSING_H
