/*! \file */ 
#ifndef _SHARED_DATA_H
#define _SHARED_DATA_H

#include <vector>
#include <map>

#include "rapidjson/document.h"

#include "options_parser.h"
#include "osm_store.h"
#include "output_object.h"
#include "mbtiles.h"
#include "pmtiles.h"
#include "tile_data.h"

///\brief Defines map single layer appearance
struct LayerDef {
	std::string name;
	uint minzoom;
	uint maxzoom;
	uint simplifyBelow;
	double simplifyLevel;
	double simplifyLength;
	double simplifyRatio;
	uint simplifyAlgo;
	uint filterBelow;
	double filterArea;
	bool sortZOrderAscending;
	uint featureLimit;
	uint featureLimitBelow;
	bool combinePoints;
	uint combineLinesBelow;
	uint combinePolygonsBelow;
	std::string source;
	std::vector<std::string> sourceColumns;
	bool allSourceColumns;
	bool indexed;
	std::string indexName;
	std::map<std::string, uint> attributeMap; // string 0, number 1, bool 2
	bool writeTo;
	// Decluttering: set after addLayer(), so these live at the end of the struct
	uint declutterBelow = 0;			// zoom below which point features are thinned out by Score()
	double declutterDistance = 40;		// how far apart (in 256px screen pixels) they should be kept
	double declutterThreshold = 0;		// score needed at the layer's minzoom (halves at each zoom)
	// Geometry repair: also set after addLayer()
	uint repairScope = 0;				// REPAIR_SIMPLIFIED_ONLY (default) or REPAIR_ALL
	
	const bool useColumn(std::string &col) {
		return allSourceColumns || (std::find(sourceColumns.begin(), sourceColumns.end(), col) != sourceColumns.end());
	}
	
	static const uint DOUGLAS_PEUCKER = 0;
	static const uint VISVALINGAM = 1;
	static const uint BUILDINGS = 2;

	// Which invalid output polygons are repaired before being written to a tile.
	// REPAIR_SIMPLIFIED_ONLY (default) repairs only polygons that simplification
	// may have broken - the historic behaviour. REPAIR_ALL is a superset: it also
	// repairs polygons that were merely clipped and quantised onto the integer
	// tile grid (e.g. ocean shapefiles). For those, a repair must not GROW the
	// polygon, because growing means a hole was filled and an island would
	// silently disappear.
	static const uint REPAIR_SIMPLIFIED_ONLY = 0;
	static const uint REPAIR_ALL = 1;
};

///\brief Defines layers used in map rendering
class LayerDefinition {

public:	
	std::vector<LayerDef> layers;				// List of layers
	std::map<std::string,uint> layerMap;				// Layer->position map
	std::vector<std::vector<uint> > layerOrder;		// Order of (grouped) layers, e.g. [ [0], [1,2,3], [4] ]

	// Define a layer (as read from the .json file)
	uint addLayer(std::string name, uint minzoom, uint maxzoom,
			uint simplifyBelow, double simplifyLevel, double simplifyLength, double simplifyRatio, uint simplifyAlgo,
			uint filterBelow, double filterArea, bool sortZOrderAscending,
			uint featureLimit, uint featureLimitBelow, bool combinePoints, uint combineLinesBelow, uint combinePolygonsBelow,
			const std::string &source,
			const std::vector<std::string> &sourceColumns,
			bool allSourceColumns,
			bool indexed,
			const std::string &indexName,
			const std::string &writeTo);
	std::vector<bool> getSortOrders();
	rapidjson::Value serialiseToJSONValue(rapidjson::Document::AllocatorType &allocator) const;
	std::string serialiseToJSON() const;
};

///\brief Config read from JSON to control behavior of program
class Config {
	
public:
	class LayerDefinition layers;
	uint baseZoom, startZoom, endZoom;
	uint mvtVersion, combineBelow;
	bool includeID, compress, gzip, highResolution;
	std::string compressOpt;
	bool clippingBoxFromJSON;
	double minLon, minLat, maxLon, maxLat;
	std::string projectName, projectVersion, projectDesc;
	std::string defaultView;

	Config();
	virtual ~Config();

	void readConfig(rapidjson::Document &jsonConfig, bool &hasClippingBox, Box &clippingBox);
	void enlargeBbox(double cMinLon, double cMaxLon, double cMinLat, double cMaxLat);
};

///\brief Data used by worker threads ::outputProc to write output
class SharedData {

public:
	const class LayerDefinition &layers;
	OptionsParser::OutputMode outputMode;
	bool mergeSqlite;
	MBTiles mbtiles;
	PMTiles pmtiles;
	std::string outputFile;

	Config &config;

	SharedData(Config &configIn, const class LayerDefinition &layers);
	virtual ~SharedData();

	void writeMBTilesProjectData();
	void writeMBTilesMetadata(rapidjson::Document const &jsonConfig);
	void writeFileMetadata(rapidjson::Document const &jsonConfig);	
	std::string pmTilesMetadata(rapidjson::Document const &jsonConfig);
	void writePMTilesBounds();
};

#endif //_SHARED_DATA_H

