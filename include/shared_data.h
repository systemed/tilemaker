/*! \file */ 
#ifndef _SHARED_DATA_H
#define _SHARED_DATA_H

#include <vector>
#include <map>

#include "rapidjson/document.h"

#include "osm_store.h"
#include "output_object.h"
#include "mbtiles.h"
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
	uint filterBelow;
	double filterArea;
	uint combinePolygonsBelow;
	std::string source;
	std::vector<std::string> sourceColumns;
	bool allSourceColumns;
	bool indexed;
	std::string indexName;
	std::map<std::string, uint> attributeMap;
	bool writeTo;
};

///\brief Defines layers used in map rendering
class LayerDefinition {

public:	
	std::vector<LayerDef> layers;				// List of layers
	std::map<std::string,uint> layerMap;				// Layer->position map
	std::vector<std::vector<uint> > layerOrder;		// Order of (grouped) layers, e.g. [ [0], [1,2,3], [4] ]

	// Define a layer (as read from the .json file)
	uint addLayer(std::string name, uint minzoom, uint maxzoom,
			uint simplifyBelow, double simplifyLevel, double simplifyLength, double simplifyRatio, 
			uint filterBelow, double filterArea, uint combinePolygonsBelow,
			const std::string &source,
			const std::vector<std::string> &sourceColumns,
			bool allSourceColumns,
			bool indexed,
			const std::string &indexName,
			const std::string &writeTo);

	rapidjson::Value serialiseToJSONValue(rapidjson::Document::AllocatorType &allocator) const;
	std::string serialiseToJSON() const;
};

///\brief Config read from JSON to control behavior of program
class Config {
	
public:
	class LayerDefinition layers;
	uint baseZoom, startZoom, endZoom;
	uint mvtVersion, combineBelow;
	bool includeID, compress, gzip;
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
	bool sqlite;
	bool mergeSqlite;
	MBTiles mbtiles;
	std::string outputFile;

	Config &config;

	SharedData(Config &configIn, const class LayerDefinition &layers);
	virtual ~SharedData();
};

#endif //_SHARED_DATA_H

