#ifndef _SHARED_DATA_H
#define _SHARED_DATA_H

#include <vector>
#include <map>

#include "rapidjson/document.h"

#include "osm_store.h"
#include "output_object.h"
#include "osm_object.h"
#include "mbtiles.h"

class SharedData
{
public:
	uint zoom;
	uint mvtVersion;
	int threadNum;
	bool clippingBoxFromJSON;
	double minLon, minLat, maxLon, maxLat;
	std::string defaultView;
	OSMStore *osmStore;
	bool includeID, compress, gzip;
	std::string compressOpt;
	std::string projectName, projectVersion, projectDesc;
	uint baseZoom, startZoom, endZoom;
	std::vector<Geometry> cachedGeometries;					// prepared boost::geometry objects (from shapefiles)
	bool verbose;
	bool sqlite;
	MBTiles mbtiles;
	std::string outputFile;
	std::map< uint, std::vector<OutputObject> > *tileIndexForZoom;

	std::vector<LayerDef> layers;				// List of layers
	std::map<std::string,uint> layerMap;				// Layer->position map
	std::vector<std::vector<uint> > layerOrder;		// Order of (grouped) layers, e.g. [ [0], [1,2,3], [4] ]

	SharedData(OSMStore *osmStore);
	virtual ~SharedData();
	void readConfig(rapidjson::Document &jsonConfig, bool hasClippingBox, Box &clippingBox,
		            std::map< uint, std::vector<OutputObject> > &tileIndex, OSMObject &osmObject);
};

#endif //_SHARED_DATA_H

