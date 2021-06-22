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
		class LayerDefinition &layers,
		OsmLuaProcessing &osmLuaProcessing) {

	if (osmLuaProcessing.canRemapShapefiles()) {
		// Create table object
		kaguya::LuaTable in_table = osmLuaProcessing.newTable();
		for (auto it : columnMap) {
			int pos = it.first;
			string key = it.second;
			switch (columnTypeMap[pos]) {
				case 1:  in_table[key] = DBFReadIntegerAttribute(dbf, recordNum, pos); break;
				case 2:  in_table[key] =  DBFReadDoubleAttribute(dbf, recordNum, pos); break;
				default: in_table[key] =  DBFReadStringAttribute(dbf, recordNum, pos); break;
			}
		}

		// Call remap function
		kaguya::LuaTable out_table = osmLuaProcessing.remapAttributes(in_table, layers.layers[oo->layer].name);

		auto &attributeStore = osmLuaProcessing.getAttributeStore();
		auto attributes = attributeStore.empty_set();

		// Write values to vector tiles
		for (auto key : out_table.keys()) {
			kaguya::LuaRef val = out_table[key];
			vector_tile::Tile_Value v;
			if (val.isType<std::string>()) {
				v.set_string_value(static_cast<std::string const&>(val));
				layers.layers[oo->layer].attributeMap[key] = 0;
			} else if (val.isType<int>()) {
				if (key=="_minzoom") { oo->setMinZoom(val); continue; }
				v.set_float_value(val);
				layers.layers[oo->layer].attributeMap[key] = 1;
			} else if (val.isType<double>()) {
				v.set_float_value(val);
				layers.layers[oo->layer].attributeMap[key] = 1;
			} else if (val.isType<bool>()) {
				v.set_bool_value(val);
				layers.layers[oo->layer].attributeMap[key] = 2;
			} else {
				// don't even think about trying to write nested tables, thank you
				std::cout << "Didn't recognise Lua output type: " << val << std::endl;
			}

			attributes->emplace(key, v, 0);
		}

		oo->setAttributeSet(attributeStore.store_set(attributes));		
	} else {
		auto &attributeStore = osmLuaProcessing.getAttributeStore();
		auto attributes = attributeStore.empty_set();

		for (auto it : columnMap) {
			int pos = it.first;
			string key = it.second;
			vector_tile::Tile_Value v;
			switch (columnTypeMap[pos]) {
				case 1:  v.set_int_value(DBFReadIntegerAttribute(dbf, recordNum, pos));
				         layers.layers[oo->layer].attributeMap[key] = 1;
				         break;
				case 2:  v.set_double_value(DBFReadDoubleAttribute(dbf, recordNum, pos));
				         layers.layers[oo->layer].attributeMap[key] = 1;
				         break;
				default: v.set_string_value(DBFReadStringAttribute(dbf, recordNum, pos));
				         layers.layers[oo->layer].attributeMap[key] = 0;
				         break;
			}
			
			attributes->emplace(key, v, 0);
		}

		oo->setAttributeSet(attributeStore.store_set(attributes));		
	}
}

// Read shapefile, and create OutputObjects for all objects within the specified bounding box
void readShapefile(const Box &clippingBox, 
				   class LayerDefinition &layers,
                   uint baseZoom, uint layerNum,
				   class ShpMemTiles &shpMemTiles,
				   OsmLuaProcessing &osmLuaProcessing)
{
	LayerDef &layer = layers.layers[layerNum];
	const string &filename = layer.source;
	const vector<string> &columns = layer.sourceColumns;
	const string &layerName = layer.name;
	bool isIndexed = layer.indexed;
	const string &indexName = layer.indexName;

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

	for (int i=0; i<numEntities; i++) {
		SHPObject* shape = SHPReadObject(shp, i);
		if(shape == nullptr) { cerr << "Error loading shape from shapefile" << endl; continue; }

		int shapeType = shape->nSHPType;	// 1=point, 3=polyline, 5=(multi)polygon [8=multipoint, 11+=3D]

		// Check shape is in clippingBox
		Box shapeBox(Point(shape->dfXMin, lat2latp(shape->dfYMin)), Point(shape->dfXMax, lat2latp(shape->dfYMax)));
		if (shapeBox.min_corner().get<0>() > clippingBox.max_corner().get<0>() ||
		    shapeBox.max_corner().get<0>() < clippingBox.min_corner().get<0>() ||
		    shapeBox.min_corner().get<1>() > clippingBox.max_corner().get<1>() ||
		    shapeBox.max_corner().get<1>() < clippingBox.min_corner().get<1>()) { continue; }

		if (shapeType==1) {
			// Points
			Point p( shape->padfX[0], lat2latp(shape->padfY[0]) );
			if (geom::within(p, clippingBox)) {

				string name;
				bool hasName = false;
				if (indexField>-1) { name=DBFReadStringAttribute(dbf, i, indexField); hasName = true;}

				auto &attributeStore = osmLuaProcessing.getAttributeStore();
				OutputObjectRef oo = shpMemTiles.AddObject(layerNum, layerName, OutputGeometryType::POINT, p, isIndexed, hasName, name, attributeStore.empty_set());

				addShapefileAttributes(dbf, oo, i, columnMap, columnTypeMap, layers, osmLuaProcessing);
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

					auto &attributeStore = osmLuaProcessing.getAttributeStore();
					OutputObjectRef oo = shpMemTiles.AddObject(layerNum, layerName, OutputGeometryType::LINESTRING, *it, isIndexed, hasName, name, attributeStore.empty_set());

					addShapefileAttributes(dbf, oo, i, columnMap, columnTypeMap, layers, osmLuaProcessing);
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
			geom::remove_spikes(multi);

			string reason;
#if BOOST_VERSION >= 105800
			if (!geom::is_valid(multi, reason)) {
				cerr << "Shapefile entity #" << i << " type " << shapeType << " is invalid. Parts:" << shape->nParts << ". Reason:" << reason;

				// Perform make_valid operation
				make_valid(multi);
				
				if (geom::is_valid(multi, reason)) {
					cerr << "... corrected";
				} else {
					cerr << "... failed to correct. Reason: " << reason;
				}
				cerr << endl;
			}
#else
			if (!geom::is_valid(multi)) { geom::correct(multi); geom::remove_spikes(multi); }
#endif
			// clip to bounding box
			MultiPolygon out;
			geom::intersection(multi, clippingBox, out);
			if (boost::size(out)>0) {

				string name;
				bool hasName = false;
				if (indexField>-1) { name=DBFReadStringAttribute(dbf, i, indexField); hasName = true;}

				// create OutputObject
				auto &attributeStore = osmLuaProcessing.getAttributeStore();
				OutputObjectRef oo = shpMemTiles.AddObject(layerNum, layerName, OutputGeometryType::POLYGON, out, isIndexed, hasName, name, attributeStore.empty_set());

				addShapefileAttributes(dbf, oo, i, columnMap, columnTypeMap, layers, osmLuaProcessing);
			}

		} else {
			// Not supported
			cerr << "Shapefile entity #" << i << " type " << shapeType << " not supported" << endl;
		}
	}

	SHPClose(shp);
	DBFClose(dbf);
}
