#include "tile_worker.h"
#include <fstream>
#include <boost/filesystem.hpp>
#include "helpers.h"
#include "write_geometry.h"
using namespace ClipperLib;
using namespace std;

void CheckNextObjectAndMerge(ObjectsAtSubLayerIterator &jt, const ObjectsAtSubLayerIterator &ooSameLayerEnd, 
	class SharedData *sharedData, const TileBbox &bbox, Geometry &g)
{
	// If a object is a polygon or a linestring that is followed by
	// other objects with the same geometry type and the same attributes,
	// the following objects are merged into the first object, by taking union of geometries.
	auto gTyp = (*jt)->geomType;
	if (gTyp == POLYGON || gTyp == CACHED_POLYGON) {
		MultiPolygon *gAcc = nullptr;
		try{
			gAcc = &boost::get<MultiPolygon>(g);
		} catch (boost::bad_get &err) {
			cerr << "Error: Polygon " << (*jt)->objectID << " has unexpected type" << endl;
			return;
		}
	
		Paths current;
		ConvertToClipper(*gAcc, current);

		while (jt+1 != ooSameLayerEnd &&
				(*(jt+1))->geomType == gTyp &&
				(*(jt+1))->attributes == (*jt)->attributes) {
			jt++;

			try {

				MultiPolygon gNew = boost::get<MultiPolygon>(jt.buildWayGeometry(bbox));
				Paths newPaths;
				ConvertToClipper(gNew, newPaths);

				Clipper cl;
				cl.StrictlySimple(true);
				cl.AddPaths(current, ptSubject, true);
				cl.AddPaths(newPaths, ptClip, true);
				Paths tmpUnion;
				cl.Execute(ctUnion, tmpUnion, pftEvenOdd, pftEvenOdd);
				swap(current, tmpUnion);
			}
			catch (std::out_of_range &err)
			{
				if (sharedData->verbose)
					cerr << "Error while processing POLYGON " << (*jt)->geomType << "," << (*jt)->objectID <<"," << err.what() << endl;
			}
		}

		ConvertFromClipper(current, *gAcc);
	}
	if (gTyp == LINESTRING || gTyp == CACHED_LINESTRING) {
		MultiLinestring *gAcc = nullptr;
		try {
		gAcc = &boost::get<MultiLinestring>(g);
		} catch (boost::bad_get &err) {
			cerr << "Error: LineString " << (*jt)->objectID << " has unexpected type" << endl;
			return;
		}
		while (jt+1 != ooSameLayerEnd &&
				(*(jt+1))->geomType == gTyp &&
				(*(jt+1))->attributes == (*jt)->attributes) {
			jt++;
			try
			{
				MultiLinestring gNew = boost::get<MultiLinestring>(jt.buildWayGeometry(bbox));
				MultiLinestring gTmp;
				geom::union_(*gAcc, gNew, gTmp);
				*gAcc = move(gTmp);
			}
			catch (std::out_of_range &err)
			{
				if (sharedData->verbose)
					cerr << "Error while processing LINESTRING " << (*jt)->geomType << "," << (*jt)->objectID <<"," << err.what() << endl;
			}
			catch (boost::bad_get &err) {
				cerr << "Error while processing LINESTRING " << (*jt)->objectID << " has unexpected type" << endl;
				continue;
			}
		}
	}
}

void ProcessObjects(const ObjectsAtSubLayerIterator &ooSameLayerBegin, const ObjectsAtSubLayerIterator &ooSameLayerEnd, 
	class SharedData *sharedData, double simplifyLevel, const TileBbox &bbox,
	vector_tile::Tile_Layer *vtLayer, vector<string> &keyList, vector<vector_tile::Tile_Value> &valueList)
{
	for (ObjectsAtSubLayerIterator jt = ooSameLayerBegin; jt != ooSameLayerEnd; ++jt) {
			
		if ((*jt)->geomType == POINT) {
			vector_tile::Tile_Feature *featurePtr = vtLayer->add_features();
			jt.buildNodeGeometry(bbox, featurePtr);
			(*jt)->writeAttributes(&keyList, &valueList, featurePtr);
			if (sharedData->config.includeID) { featurePtr->set_id((*jt)->objectID); }
		} else {
			Geometry g;
			try {
				g = jt.buildWayGeometry(bbox);
			}
			catch (std::out_of_range &err)
			{
				if (sharedData->verbose)
					cerr << "Error while processing geometry " << (*jt)->geomType << "," << (*jt)->objectID <<"," << err.what() << endl;
				continue;
			}

			//This may increment the jt iterator
			if(sharedData->config.combineSimilarObjs)
				CheckNextObjectAndMerge(jt, ooSameLayerEnd, sharedData, bbox, g);

			vector_tile::Tile_Feature *featurePtr = vtLayer->add_features();
			WriteGeometryVisitor w(&bbox, featurePtr, simplifyLevel);
			boost::apply_visitor(w, g);
			if (featurePtr->geometry_size()==0) { vtLayer->mutable_features()->RemoveLast(); continue; }
			(*jt)->writeAttributes(&keyList, &valueList, featurePtr);
			if (sharedData->config.includeID) { featurePtr->set_id((*jt)->objectID); }

		}
	}
}

void ProcessLayer(uint zoom, const TilesAtZoomIterator &it, vector_tile::Tile &tile, 
	const TileBbox &bbox, const std::vector<uint> &ltx, class SharedData *sharedData)
{
	TileCoordinates index = it.GetCoordinates();

	vector<string> keyList;
	vector<vector_tile::Tile_Value> valueList;
	vector_tile::Tile_Layer *vtLayer = tile.add_layers();

	//TileCoordinate tileX = index.x;
	TileCoordinate tileY = index.y;

	// Loop through sub-layers
	for (auto mt = ltx.begin(); mt != ltx.end(); ++mt) {
		uint layerNum = *mt;
		const LayerDef &ld = sharedData->layers.layers[layerNum];
		if (zoom<ld.minzoom || zoom>ld.maxzoom) { continue; }
		double simplifyLevel = 0.0;
		if (zoom < ld.simplifyBelow) {
			if (ld.simplifyLength > 0) {
				double latp = (tiley2latp(tileY, zoom) + tiley2latp(tileY+1, zoom)) / 2;
				simplifyLevel = meter2degp(ld.simplifyLength, latp);
			} else {
				simplifyLevel = ld.simplifyLevel;
			}
			simplifyLevel *= pow(ld.simplifyRatio, (ld.simplifyBelow-1) - zoom);
		}

		ObjectsAtSubLayerConstItPair ooListSameLayer = it.GetObjectsAtSubLayer(layerNum);
		// Loop through output objects
		ProcessObjects(ooListSameLayer.first, ooListSameLayer.second, sharedData, simplifyLevel, bbox, vtLayer, keyList, valueList);
	}

	// If there are any objects, then add tags
	if (vtLayer->features_size()>0) {
		vtLayer->set_name(sharedData->layers.layers[ltx.at(0)].name);
		vtLayer->set_version(sharedData->config.mvtVersion);
		vtLayer->set_extent(4096);
		for (uint j=0; j<keyList.size()  ; j++) {
			vtLayer->add_keys(keyList[j]);
		}
		for (uint j=0; j<valueList.size(); j++) { 
			vector_tile::Tile_Value *v = vtLayer->add_values();
			*v = valueList[j];
		}
	} else {
		tile.mutable_layers()->RemoveLast();
	}
}

int outputProc(uint threadId, class SharedData *sharedData)
{

	// Loop through tiles
	uint tc = 0;
	uint zoom = sharedData->zoom;
	for (auto it = sharedData->tileData.GetTilesAtZoomBegin(); it != sharedData->tileData.GetTilesAtZoomEnd(); ++it) {
		uint interval = 100;
		if (zoom<9) { interval=1; } else if (zoom<11) { interval=10; }
		if (threadId == 0 && (tc % interval) == 0) {
			cout << "Zoom level " << zoom << ", writing tile " << tc << " of " << sharedData->tileData.GetTilesAtZoomSize() << "               \r";
			cout.flush();
		}
		if (tc++ % sharedData->threadNum != threadId) continue;

		// Create tile
		vector_tile::Tile tile;
		TileBbox bbox(it.GetCoordinates(), zoom);
		if (sharedData->config.clippingBoxFromJSON && (sharedData->config.maxLon<=bbox.minLon 
			|| sharedData->config.minLon>=bbox.maxLon || sharedData->config.maxLat<=bbox.minLat 
			|| sharedData->config.minLat>=bbox.maxLat)) { continue; }

		// Loop through layers
		for (auto lt = sharedData->layers.layerOrder.begin(); lt != sharedData->layers.layerOrder.end(); ++lt) {
			ProcessLayer(zoom, it, tile, bbox, *lt, sharedData);
		}

		// Write to file or sqlite

		string data, compressed;
		if (sharedData->sqlite) {
			// Write to sqlite
			tile.SerializeToString(&data);
			if (sharedData->config.compress) { compressed = compress_string(data, Z_DEFAULT_COMPRESSION, sharedData->config.gzip); }
			sharedData->mbtiles.saveTile(zoom, bbox.index.x, bbox.index.y, sharedData->config.compress ? &compressed : &data);

		} else {
			// Write to file
			stringstream dirname, filename;
			dirname  << sharedData->outputFile << "/" << zoom << "/" << bbox.index.x;
			filename << sharedData->outputFile << "/" << zoom << "/" << bbox.index.x << "/" << bbox.index.y << ".pbf";
			boost::filesystem::create_directories(dirname.str());
			fstream outfile(filename.str(), ios::out | ios::trunc | ios::binary);
			if (sharedData->config.compress) {
				tile.SerializeToString(&data);
				outfile << compress_string(data, Z_DEFAULT_COMPRESSION, sharedData->config.gzip);
			} else {
				if (!tile.SerializeToOstream(&outfile)) { cerr << "Couldn't write to " << filename.str() << endl; return -1; }
			}
			outfile.close();
		}
	}
	return 0;

}

