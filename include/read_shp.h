#ifndef _READ_SHP_H
#define _READ_SHP_H

#include <unordered_map>
#include <string>
#include <vector>
#include <map>
#include "geomtypes.h"
#include "output_object.h"
#include "osm_object.h"

// Shapelib
#include "shapefil.h"

void fillPointArrayFromShapefile(std::vector<Point> *points, SHPObject *shape, uint part);

/// Add an OutputObject to all tiles between min/max lat/lon
void addToTileIndexByBbox(OutputObject &oo, std::map< uint64_t, std::vector<OutputObject> > &tileIndex, uint baseZoom,
                          double minLon, double minLatp, double maxLon, double maxLatp);

/// Add an OutputObject to all tiles along a polyline
void addToTileIndexPolyline(OutputObject &oo, std::map< uint64_t, std::vector<OutputObject> > &tileIndex, uint baseZoom, const Linestring &ls);

/// Read requested attributes from a shapefile, and encode into an OutputObject
void addShapefileAttributes(DBFHandle &dbf, OutputObject &oo, int recordNum, std::unordered_map<int,std::string> &columnMap, std::unordered_map<int,int> &columnTypeMap);

/// Read shapefile, and create OutputObjects for all objects within the specified bounding box
void readShapefile(std::string filename,
                   std::vector<std::string> &columns,
                   const Box &clippingBox,
                   std::map< uint64_t, std::vector<OutputObject> > &tileIndex,
                   std::vector<Geometry> &cachedGeometries,
                   OSMObject &osmObject,
                   uint baseZoom, uint layerNum, const std::string &layerName,
                   bool isIndexed,
                   const std::string &indexName);

#endif //_READ_SHP_H

