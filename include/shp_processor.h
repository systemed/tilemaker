/*! \file */ 
#ifndef _SHP_PROCESSOR_H
#define _SHP_PROCESSOR_H

#include <unordered_map>
#include <string>
#include <vector>
#include <map>
#include "geom.h"
#include "output_object.h"
#include "osm_lua_processing.h"
#include "attribute_store.h"

// Shapelib
#include "shapefil.h"

class ShpProcessor {

public:
	ShpProcessor(Box &clippingBox, 
	             uint threadNum,
	             class ShpMemTiles &shpMemTiles,
	             OsmLuaProcessing &osmLuaProcessing) : 
		 clippingBox(clippingBox), threadNum(threadNum),
		 shpMemTiles(shpMemTiles), osmLuaProcessing(osmLuaProcessing)
	{}

	// Read shapefile, and create OutputObjects for all objects within the specified bounding box
	void read(class LayerDef &layer, uint layerNum);

private:	
	Box clippingBox;
	unsigned threadNum;
	ShpMemTiles &shpMemTiles;
	OsmLuaProcessing &osmLuaProcessing;
	std::mutex attributeMutex;

	void fillPointArrayFromShapefile(std::vector<Point> *points, SHPObject *shape, uint part);

	// Read requested attributes from a shapefile, and encode into an OutputObject
	AttributeIndex readShapefileAttributes(DBFHandle &dbf, int recordNum, 
	                                       std::unordered_map<int,std::string> &columnMap,
	                                       std::unordered_map<int,int> &columnTypeMap,
	                                       LayerDef &layer, uint &minzoom);

	// Process an individual shapefile record
	void processShapeGeometry(SHPObject* shape, AttributeIndex attrIdx, 
	                          const LayerDef &layer, uint layerNum, bool hasName, const std::string &name);
};

#endif //_SHP_PROCESSOR_H

