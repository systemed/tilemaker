#include "shp_processor.h"

#include <boost/asio/thread_pool.hpp>
#include <boost/asio/post.hpp>

extern bool verbose;

using namespace std;
namespace geom = boost::geometry;

/*
	Read shapefiles into Boost geometries
*/

void ShpProcessor::fillPointArrayFromShapefile(vector<Point> *points, SHPObject *shape, uint part) {
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
// columnTypeMap: 0 string, 1 int, 2 double, 3 boolean
AttributeIndex ShpProcessor::readShapefileAttributes(
		DBFHandle &dbf,
		int recordNum, unordered_map<int,string> &columnMap, unordered_map<int,int> &columnTypeMap,
		LayerDef &layer, uint &minzoom) {

	std::lock_guard<std::mutex> lock(attributeMutex);
	AttributeStore& attributeStore = osmLuaProcessing.getAttributeStore();

	AttributeSet attributes;
	if (osmLuaProcessing.canRemapShapefiles()) {
		// Create table object
		kaguya::LuaTable in_table = osmLuaProcessing.newTable();
		for (auto it : columnMap) {
			int pos = it.first;
			string key = it.second;
			switch (columnTypeMap[pos]) {
				case 1:  in_table[key] = DBFReadIntegerAttribute(dbf, recordNum, pos); break;
				case 2:  in_table[key] =  DBFReadDoubleAttribute(dbf, recordNum, pos); break;
				case 3:  in_table[key] = strcmp(DBFReadStringAttribute(dbf, recordNum, pos), "T")==0; break;
				default: in_table[key] =  DBFReadStringAttribute(dbf, recordNum, pos); break;
			}
		}

		// Call remap function
		kaguya::LuaTable out_table = osmLuaProcessing.remapAttributes(in_table, layer.name);

		// Write values to vector tiles
		for (auto key : out_table.keys()) {
			kaguya::LuaRef val = out_table[key];
			if (val.isType<std::string>()) {
				attributeStore.addAttribute(attributes, key, static_cast<const std::string&>(val), 0);
				layer.attributeMap[key] = 0;
			} else if (val.isType<int>()) {
				if (key=="_minzoom") { minzoom=val; continue; }
				attributeStore.addAttribute(attributes, key, (int)val, 0);
				layer.attributeMap[key] = 1;
			} else if (val.isType<double>()) {
				attributeStore.addAttribute(attributes, key, (double)val, 0);
				layer.attributeMap[key] = 1;
			} else if (val.isType<bool>()) {
				attributeStore.addAttribute(attributes, key, (bool)val, 0);
				layer.attributeMap[key] = 2;
			} else {
				// don't even think about trying to write nested tables, thank you
				std::cout << "Didn't recognise Lua output type: " << val << std::endl;
			}
		}
	} else {
		for (auto it : columnMap) {
			int pos = it.first;
			string key = it.second;
			switch (columnTypeMap[pos]) {
				case 1:  attributeStore.addAttribute(attributes, key, (int)DBFReadIntegerAttribute(dbf, recordNum, pos), 0);
				         layer.attributeMap[key] = 1;
				         break;
				case 2:  attributeStore.addAttribute(attributes, key, (double)DBFReadDoubleAttribute(dbf, recordNum, pos), 0);
				         layer.attributeMap[key] = 1;
				         break;
				case 3:  attributeStore.addAttribute(attributes, key, strcmp(DBFReadStringAttribute(dbf, recordNum, pos), "T")==0, 0);
				         layer.attributeMap[key] = 2;
				         break;
				default: attributeStore.addAttribute(attributes, key, static_cast<const std::string&>(DBFReadStringAttribute(dbf, recordNum, pos)), 0);
				         layer.attributeMap[key] = 0;
				         break;
			}
		}
	}
	return attributeStore.add(attributes);
}

// Read shapefile, and create OutputObjects for all objects within the specified bounding box
void ShpProcessor::read(class LayerDef &layer, uint layerNum)
{
	const string &filename = layer.source;
	const vector<string> &columns = layer.sourceColumns;
	const string &indexName = layer.indexName;

	// open shapefile
	SHPHandle shp = SHPOpen(filename.c_str(), "rb");
	DBFHandle dbf = DBFOpen(filename.c_str(), "rb");
	if(shp == nullptr || dbf == nullptr)
		return;
	int numEntities=0, shpType=0;
	double adfMinBound[4], adfMaxBound[4];
	SHPGetInfo(shp, &numEntities, &shpType, adfMinBound, adfMaxBound);
	
	// prepare columns
	unordered_map<int,string> columnMap;
	unordered_map<int,int> columnTypeMap;
	if (layer.allSourceColumns) {
		for (size_t i=0; i<DBFGetFieldCount(dbf); i++) {
			char name[12];
			columnTypeMap[i] = DBFGetFieldInfo(dbf,i,name,NULL,NULL);
			columnMap[i] = string(name);
		}
	} else {
		for (size_t i=0; i<columns.size(); i++) {
			int dbfLoc = DBFGetFieldIndex(dbf,columns[i].c_str());
			if (dbfLoc>-1) { 
				columnMap[dbfLoc]=columns[i];
				columnTypeMap[dbfLoc]=DBFGetFieldInfo(dbf,dbfLoc,NULL,NULL,NULL);
			}
		}
	}
	int indexField=-1;
	if (indexName!="") { indexField = DBFGetFieldIndex(dbf,indexName.c_str()); }

	boost::asio::thread_pool pool(threadNum);
	for (int i=0; i<numEntities; i++) {
		SHPObject* shape = SHPReadObject(shp, i);
		if(shape == nullptr) { cerr << "Error loading shape from shapefile" << endl; continue; }

		// Check shape is in clippingBox
		Box shapeBox(Point(shape->dfXMin, lat2latp(shape->dfYMin)), Point(shape->dfXMax, lat2latp(shape->dfYMax)));
		if (shapeBox.min_corner().get<0>() > clippingBox.max_corner().get<0>() ||
		    shapeBox.max_corner().get<0>() < clippingBox.min_corner().get<0>() ||
		    shapeBox.min_corner().get<1>() > clippingBox.max_corner().get<1>() ||
		    shapeBox.max_corner().get<1>() < clippingBox.min_corner().get<1>()) {
			SHPDestroyObject(shape);
			continue;
		}

		boost::asio::post(pool, [&, i, shape]() {
			// process attributes
			string name;
			bool hasName = false;
			if (indexField>-1) { 
				std::lock_guard<std::mutex> lock(attributeMutex);
				name=DBFReadStringAttribute(dbf, i, indexField); hasName = true;
			}
			AttributeIndex attrIdx = readShapefileAttributes(dbf, i, columnMap, columnTypeMap, layer, layer.minzoom);
			// process geometry
			processShapeGeometry(shape, attrIdx, layer, layerNum, hasName, name);
			SHPDestroyObject(shape);
		});
	}
	pool.join();
	SHPClose(shp);
	DBFClose(dbf);
}

void ShpProcessor::processShapeGeometry(SHPObject* shape, AttributeIndex attrIdx,
                                        const LayerDef &layer, uint layerNum, bool hasName, const string &name) {
	int shapeType = shape->nSHPType;	// 1=point, 3=polyline, 5=(multi)polygon [8=multipoint, 11+=3D]
	int minzoom = layer.minzoom;

	if (shapeType==1 || shapeType==11 || shapeType==21) {
		// Points
		Point p( shape->padfX[0], lat2latp(shape->padfY[0]) );
		if (geom::within(p, clippingBox)) {
			shpMemTiles.StoreGeometry(layerNum, layer.name, POINT_, p, layer.indexed, hasName, name, minzoom, attrIdx);
		}

	} else if (shapeType==8 || shapeType==18 || shapeType==28) {
		// Multipoint
		for (uint i=0; i<shape->nVertices; i++) {
			Point p( shape->padfX[i], lat2latp(shape->padfY[i]) );
			if (geom::within(p, clippingBox)) {
				shpMemTiles.StoreGeometry(layerNum, layer.name, POINT_, p, layer.indexed, hasName, name, minzoom, attrIdx);
			}
		}

	} else if (shapeType==3 || shapeType==13 || shapeType==23) {
		// (Multi)-polylines
		// Due to https://svn.boost.org/trac/boost/ticket/11268, we can't clip a MultiLinestring with Boost 1.56-1.58, 
		// so we need to create everything as polylines and clip individually :(
		vector<Point> points;
		for (int j=0; j<shape->nParts; j++) {
			Linestring ls;
			fillPointArrayFromShapefile(&points, shape, j);
			geom::assign_points(ls, points);
			MultiLinestring out;
			geom::intersection(ls, clippingBox, out);
			for (MultiLinestring::const_iterator it = out.begin(); it != out.end(); ++it) {
				shpMemTiles.StoreGeometry(layerNum, layer.name, LINESTRING_, *it, layer.indexed, hasName, name, minzoom, attrIdx);
			}
		}

	} else if (shapeType==5 || shapeType==15 || shapeType==25) {
		// (Multi)-polygons
		MultiPolygon multi;
		Polygon poly;
		Ring ring;
		int nInteriorRings = 0;
		vector<Point> points;

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
		geom::remove_spikes(multi);

		// Make valid if needs be
		string reason;
		if (!geom::is_valid(multi, reason)) {
			if (verbose) cerr << "Shapefile entity " << shape->nShapeId << " type " << shapeType << " is invalid. Parts:" << shape->nParts << ". Reason:" << reason;
			make_valid(multi);
			if (verbose) {
				if (geom::is_valid(multi, reason)) { cerr << "... corrected"; }
				                              else { cerr << "... failed to correct. Reason: " << reason; }
				cerr << endl;
			}
		}
		// clip to bounding box
		MultiPolygon out;
		geom::intersection(multi, clippingBox, out);
		if (boost::size(out)>0) {
			shpMemTiles.StoreGeometry(layerNum, layer.name, POLYGON_, out, layer.indexed, hasName, name, minzoom, attrIdx);
		}

	} else {
		// Not supported
		cerr << "Shapefile entity #" << shape->nShapeId << " type " << shapeType << " not supported" << endl;
	}
}
