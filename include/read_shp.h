/*! \file */ 
#ifndef _READ_SHP_H
#define _READ_SHP_H

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

void fillPointArrayFromShapefile(std::vector<Point> *points, SHPObject *shape, uint part);

/// Read requested attributes from a shapefile, and encode into an OutputObject
AttributeIndex readShapefileAttributes(DBFHandle &dbf, int recordNum, 
                                       std::unordered_map<int,std::string> &columnMap,
                                       std::unordered_map<int,int> &columnTypeMap,
                                       LayerDef &layer,
                                       OsmLuaProcessing &osmLuaProcessing, uint &minzoom);

/// Read shapefile, and create OutputObjects for all objects within the specified bounding box
void readShapefile(const Box &clippingBox,
                   class LayerDefinition &layers,
                   uint baseZoom, uint layerNum,
                   uint threadNum,
                   class ShpMemTiles &shpMemTiles,
                   OsmLuaProcessing &osmLuaProcessing);

// Process an individual shapefile record
void processShapeGeometry(SHPObject* shape, AttributeIndex attrIdx, ShpMemTiles &shpMemTiles,
                          const Box &clippingBox, const LayerDef &layer, uint layerNum, bool hasName, const std::string &name);

#endif //_READ_SHP_H

