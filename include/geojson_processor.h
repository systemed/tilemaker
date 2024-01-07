/*! \file */ 
#ifndef _GEOJSON_PROCESSOR_H
#define _GEOJSON_PROCESSOR_H

#include <unordered_map>
#include <string>
#include <vector>
#include <map>
#include "geom.h"
#include "output_object.h"
#include "osm_lua_processing.h"
#include "attribute_store.h"

class GeoJSONProcessor {

public:
	GeoJSONProcessor(Box &clippingBox, 
	                 uint threadNum,
	                 class ShpMemTiles &shpMemTiles,
	                 OsmLuaProcessing &osmLuaProcessing) : 
		 clippingBox(clippingBox), threadNum(threadNum),
		 shpMemTiles(shpMemTiles), osmLuaProcessing(osmLuaProcessing)
	{}

	void read(class LayerDef &layer, uint layerNum);

private:	
	Box clippingBox;
	unsigned threadNum;
	ShpMemTiles &shpMemTiles;
	OsmLuaProcessing &osmLuaProcessing;
	std::mutex attributeMutex;

	template <bool Flag, typename T>
	void processFeature(rapidjson::GenericObject<Flag, T> feature, class LayerDef &layer, uint layerNum);

	template <bool Flag, typename T>
	Polygon polygonFromGeoJSONArray(const rapidjson::GenericArray<Flag, T> &coords);

	template <bool Flag, typename T>
	std::vector<Point> pointsFromGeoJSONArray(const rapidjson::GenericArray<Flag, T> &arr);
	
	AttributeIndex readProperties(const rapidjson::Value &pr, bool &hasName, std::string &name, LayerDef &layer, unsigned &minzoom);
};

#endif //_GEOJSON_PROCESSOR_H
