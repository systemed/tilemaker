#include "read_shp.h"
using namespace std;
namespace geom = boost::geometry;

/*
	Read shapefiles into Boost geometries
*/

void fillPointArrayFromShapefile(vector<Point> *points, SHPObject *shape, uint part) {
	uint start = shape->panPartStart[part];
	uint end   = (int(part)==shape->nParts-1) ? shape->nVertices : shape->panPartStart[part+1];
    double* const x = shape->padfX;
    double* const y = shape->padfY;
	points->clear(); if (points->capacity() < (end-start)+1) { points->reserve(end-start+1); }
	double prevx = 1000;
	double prevy = 1000;
	for (uint i=start; i<end; i++) {
		y[i] = fmin(fmax(y[i], MinLat),MaxLat);	// To avoid infinite latp
		double latp = lat2latp(y[i]);
		// skip duplicated point
		if ((i == end - 1 && (x[i] != prevx || latp != prevy)) ||
		    (fabs(x[i] - prevx) >= 0.00000001 || fabs(latp - prevy) >= 0.00000001)) {
			points->emplace_back(geom::make<Point>(x[i], latp));
			prevx = x[i];
			prevy = latp;
		}
		points->emplace_back(geom::make<Point>(x[i], lat2latp(y[i])));
	}
}

// Read requested attributes from a shapefile, and encode into an OutputObject
void addShapefileAttributes(
		DBFHandle &dbf,
		OutputObjectRef &oo,
		int recordNum, unordered_map<int,string> &columnMap, unordered_map<int,int> &columnTypeMap,
		OSMObject &osmObject) {

	for (auto it : columnMap) {
		int pos = it.first;
		string key = it.second;
		vector_tile::Tile_Value v;
		switch (columnTypeMap[pos]) {
			case 1:  v.set_int_value(DBFReadIntegerAttribute(dbf, recordNum, pos));
			         osmObject.setVectorLayerMetadata(oo->layer, key, 1);
			         break;
			case 2:  v.set_double_value(DBFReadDoubleAttribute(dbf, recordNum, pos));
			         osmObject.setVectorLayerMetadata(oo->layer, key, 1);
			         break;
			default: v.set_string_value(DBFReadStringAttribute(dbf, recordNum, pos));
			         osmObject.setVectorLayerMetadata(oo->layer, key, 3);
			         break;
		}
		oo->addAttribute(key, v);
	}
}


// Read shapefile, and create OutputObjects for all objects within the specified bounding box
void readShapefile(string filename, 
                   vector<string> &columns,
                   const Box &clippingBox, 
                   TileIndex &tileIndex, 
                   vector<Geometry> &cachedGeometries,
				   OSMObject &osmObject,
                   uint baseZoom, uint layerNum, const string &layerName,
                   bool isIndexed,
				   const string &indexName,
				   class ShpMemTiles &shpMemTiles) 
{
	// open shapefile
	SHPHandle shp = SHPOpen(filename.c_str(), "rb");
	DBFHandle dbf = DBFOpen(filename.c_str(), "rb");
	if(shp == nullptr || dbf == nullptr)
		return;
	int numEntities=0, shpType=0;
	vector<Point> points;
	double adfMinBound[4], adfMaxBound[4];
	SHPGetInfo(shp, &numEntities, &shpType, adfMinBound, adfMaxBound);
	
	// prepare columns
	unordered_map<int,string> columnMap;
	unordered_map<int,int> columnTypeMap;
	for (size_t i=0; i<columns.size(); i++) {
		int dbfLoc = DBFGetFieldIndex(dbf,columns[i].c_str());
		if (dbfLoc>-1) { 
			columnMap[dbfLoc]=columns[i];
			columnTypeMap[dbfLoc]=DBFGetFieldInfo(dbf,dbfLoc,NULL,NULL,NULL);
		}
	}
	int indexField=-1;
	if (indexName!="") { indexField = DBFGetFieldIndex(dbf,indexName.c_str()); }

	for (int i=0; i<numEntities; i++) {
		SHPObject* shape = SHPReadObject(shp, i);
		int shapeType = shape->nSHPType;	// 1=point, 3=polyline, 5=(multi)polygon [8=multipoint, 11+=3D]
	
		if (shapeType==1) {
			// Points
			Point p( shape->padfX[0], lat2latp(shape->padfY[0]) );
			if (geom::within(p, clippingBox)) {

				string name;
				bool hasName = false;
				if (indexField>-1) { name=DBFReadStringAttribute(dbf, i, indexField); hasName = true;}

				OutputObjectRef oo = shpMemTiles.AddObjectedToIndex(layerNum, layerName, CACHED_POINT, p, isIndexed, hasName, name);

				std::make_shared<OutputObjectCached>(CACHED_POINT, layerNum, cachedGeometries.size()-1, cachedGeometries);
				addShapefileAttributes(dbf,oo,i,columnMap,columnTypeMap,osmObject);
			}

		} else if (shapeType==3) {
			// (Multi)-polylines
			// Due to https://svn.boost.org/trac/boost/ticket/11268, we can't clip a MultiLinestring with Boost 1.56-1.58, 
			// so we need to create everything as polylines and clip individually :(
			for (int j=0; j<shape->nParts; j++) {
				Linestring ls;
				fillPointArrayFromShapefile(&points, shape, j);
				geom::assign_points(ls, points);
				MultiLinestring out;
				geom::intersection(ls, clippingBox, out);
				for (MultiLinestring::const_iterator it = out.begin(); it != out.end(); ++it) {

					string name;
					bool hasName = false;
					if (indexField>-1) { name=DBFReadStringAttribute(dbf, i, indexField); hasName = true;}

					OutputObjectRef oo = shpMemTiles.AddObjectedToIndex(layerNum, layerName, CACHED_LINESTRING, *it, isIndexed, hasName, name);

					addShapefileAttributes(dbf,oo,i,columnMap,columnTypeMap,osmObject);
				}
			}

		} else if (shapeType==5) {
			// (Multi)-polygons
			MultiPolygon multi;
			Polygon poly;
			Ring ring;
			int nInteriorRings = 0;

			// To avoid expensive computations, we assume the shapefile has been pre-processed
			// such that each polygon's exterior ring is immediately followed by its interior rings.
			for (int j=0; j<shape->nParts; j++) {
				fillPointArrayFromShapefile(&points, shape, j);
				// Read points into a ring
				ring.clear();
				geom::append(ring, points);

				if (j == 0) {
					// We assume the first part is an exterior ring of the first polygon.
					geom::append(poly, ring);
				}
				else if (geom::area(ring) > 0.0) {
					// This part has clockwise orientation - an exterior ring.
					// Start a new polygon.
					multi.push_back(poly);
					poly.clear();
					nInteriorRings = 0;
					geom::append(poly, ring);
				} else {
					// This part has anti-clockwise orientation.
					// Add another interior ring to the current polygon.
					nInteriorRings++;
					geom::interior_rings(poly).resize(nInteriorRings);
					geom::append(poly, ring, nInteriorRings - 1);
				}
			}
			// All parts read. Add the last polygon.
			multi.push_back(poly);

			string reason;
			if (!geom::is_valid(multi, reason)) {
				cerr << "Shapefile entity #" << i << " type " << shapeType << " is invalid. Parts:" << shape->nParts << ". Reason:" << reason;
				geom::correct(multi);
				geom::remove_spikes(multi);	// water polygon shapefile has many spikes
				if (geom::is_valid(multi, reason)) {
					cerr << "... corrected";
				} else {
					cerr << "... failed to correct. Reason: " << reason;
				}
				cerr << endl;
			}
			// clip to bounding box
			MultiPolygon out;
			geom::intersection(multi, clippingBox, out);
			if (boost::size(out)>0) {

				string name;
				bool hasName = false;
				if (indexField>-1) { name=DBFReadStringAttribute(dbf, i, indexField); hasName = true;}

				// create OutputObject
				OutputObjectRef oo = shpMemTiles.AddObjectedToIndex(layerNum, layerName, CACHED_POLYGON, out, isIndexed, hasName, name);

				addShapefileAttributes(dbf,oo,i,columnMap,columnTypeMap,osmObject);
			}

		} else {
			// Not supported
			cerr << "Shapefile entity #" << i << " type " << shapeType << " not supported" << endl;
		}
	}

	SHPClose(shp);
	DBFClose(dbf);
}
