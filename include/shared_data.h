#ifndef _SHARED_DATA_H
#define _SHARED_DATA_H

#include <vector>
#include <map>

#include "rapidjson/document.h"

#include "osm_store.h"
#include "output_object.h"
#include "osm_object.h"
#include "mbtiles.h"

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
	std::vector<Geometry> cachedGeometries;					// prepared boost::geometry objects (from shapefiles)

	Config();
	virtual ~Config();

	void readConfig(rapidjson::Document &jsonConfig, bool hasClippingBox, Box &clippingBox,
		            std::map< uint, std::vector<OutputObject> > &tileIndex, OSMObject &osmObject);
};

class SharedData
{
public:
	uint zoom;
	int threadNum;
	OSMStore *osmStore;
	bool verbose;
	bool sqlite;
	MBTiles mbtiles;
	std::string outputFile;
	std::map< uint, std::vector<OutputObject> > *tileIndexForZoom;

	class Config config;

	SharedData(OSMStore *osmStore);
	virtual ~SharedData();
};

#endif //_SHARED_DATA_H

