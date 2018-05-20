#ifndef _SHARED_DATA_H
#define _SHARED_DATA_H

#include <vector>
#include <map>

#include "rapidjson/document.h"

#include "osm_store.h"
#include "output_object.h"
#include "osm_object.h"
#include "mbtiles.h"

struct LayerDef {
	std::string name;
	uint minzoom;
	uint maxzoom;
	uint simplifyBelow;
	double simplifyLevel;
	double simplifyLength;
	double simplifyRatio;
	std::string source;
	std::vector<std::string> sourceColumns;
	bool indexed;
	std::string indexName;
	std::map<std::string, uint> attributeMap;
};

class Config
{
public:
	std::vector<LayerDef> layers;				// List of layers
	std::map<std::string,uint> layerMap;				// Layer->position map
	std::vector<std::vector<uint> > layerOrder;		// Order of (grouped) layers, e.g. [ [0], [1,2,3], [4] ]
	uint baseZoom, startZoom, endZoom;
	uint mvtVersion;
	bool includeID, compress, gzip;
	std::string compressOpt;
	bool clippingBoxFromJSON;
	double minLon, minLat, maxLon, maxLat;
	std::string projectName, projectVersion, projectDesc;
	std::string defaultView;

	Config();
	virtual ~Config();

	void readConfig(rapidjson::Document &jsonConfig, bool &hasClippingBox, Box &clippingBox);

	// Define a layer (as read from the .json file)
	uint addLayer(std::string name, uint minzoom, uint maxzoom,
			uint simplifyBelow, double simplifyLevel, double simplifyLength, double simplifyRatio, 
			const std::string &source,
			const std::vector<std::string> &sourceColumns,
			bool indexed,
			const std::string &indexName,	
			const std::string &writeTo);

};

class SharedData
{
public:
	uint zoom;

	///Number of worker threads to create
	int threadNum;
	const OSMStore *osmStore;
	bool verbose;
	bool sqlite;
	MBTiles mbtiles;
	std::string outputFile;
	const std::map< uint, std::vector<OutputObject> > *tileIndexForZoom;
	const std::vector<Geometry> *cachedGeometries;

	const class Config &config;

	SharedData(class Config &configIn, OSMStore *osmStore);
	virtual ~SharedData();
};

#endif //_SHARED_DATA_H

