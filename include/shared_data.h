#ifndef _SHARED_DATA_H
#define _SHARED_DATA_H

#include <vector>
#include <map>
#include "kaguya.hpp"

#include "rapidjson/document.h"

#include "osm_store.h"
#include "output_object.h"
#include "osm_object.h"
#include "mbtiles.h"

extern kaguya::State luaState;

class SharedData
{
public:
	uint zoom;
	uint mvtVersion;
	int threadNum;
	bool clippingBoxFromJSON;
	double minLon, minLat, maxLon, maxLat;
	std::string defaultView;
	OSMObject osmObject;
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

	SharedData(kaguya::State *luaPtr, std::map< std::string, RTree> *idxPtr, std::map<uint,std::string> *namePtr, OSMStore *osmStore);
	virtual ~SharedData();
	void readConfig(rapidjson::Document &jsonConfig, bool hasClippingBox, Box &clippingBox,
		            std::map< uint, std::vector<OutputObject> > &tileIndex);
};

#endif //_SHARED_DATA_H

