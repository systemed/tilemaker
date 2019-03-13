/*! \file */ 
#ifndef _READ_SHP_H
#define _READ_SHP_H

#include <unordered_map>
#include <string>
#include <vector>
#include <map>
#include "geomtypes.h"
#include "output_object.h"
#include "osm_lua_processing.h"

// Shapelib
#include "shapefil.h"

void fillPointArrayFromShapefile(std::vector<Point> *points, SHPObject *shape, uint part);

void prepareShapefile(class LayerDefinition &layers,
                   uint baseZoom, uint layerNum);

/// Read shapefile, and create OutputObjects for all objects within the specified bounding box
void readShapefile(const Box &clippingBox,
                   const class LayerDefinition &layers,
                   uint baseZoom, uint layerNum,
				   class TileIndexCached &outObj);

#endif //_READ_SHP_H

