#include "tile_worker.h"
#include <fstream>
#include <boost/filesystem.hpp>
#include "helpers.h"
#include "write_geometry.h"
using namespace ClipperLib;
using namespace std;

typedef vector<OutputObject>::const_iterator OutputObjectsConstIt;
typedef pair<OutputObjectsConstIt,OutputObjectsConstIt> OutputObjectsConstItPair;

void CheckNextObjectAndMerge(OutputObjectsConstIt &jt, const OutputObjectsConstIt &ooSameLayerEnd, 
	class SharedData *sharedData, TileBbox &bbox, Geometry &g)
{
	// If a object is a polygon or a linestring that is followed by
	// other objects with the same geometry type and the same attributes,
	// the following objects are merged into the first object, by taking union of geometries.
	auto gTyp = jt->geomType;
	if (gTyp == POLYGON || gTyp == CACHED_POLYGON) {
		MultiPolygon *gAcc = nullptr;
		try{
			gAcc = &boost::get<MultiPolygon>(g);
		} catch (boost::bad_get &err) {
			cerr << "Error: Polygon " << jt->objectID << " has unexpected type" << endl;
			return;
		}
	
		Paths current;
		ConvertToClipper(*gAcc, current);

		while (jt+1 != ooSameLayerEnd &&
				(jt+1)->geomType == gTyp &&
				(jt+1)->attributes == jt->attributes) {
			jt++;

			try {

				MultiPolygon gNew = boost::get<MultiPolygon>(jt->buildWayGeometry(*sharedData->osmStore, &bbox, sharedData->cachedGeometries));
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
					cerr << "Error while processing POLYGON " << jt->geomType << "," << jt->objectID <<"," << err.what() << endl;
			}
		}

		ConvertFromClipper(current, *gAcc);
	}
	if (gTyp == LINESTRING || gTyp == CACHED_LINESTRING) {
		MultiLinestring *gAcc = nullptr;
		try {
		gAcc = &boost::get<MultiLinestring>(g);
		} catch (boost::bad_get &err) {
			cerr << "Error: LineString " << jt->objectID << " has unexpected type" << endl;
			return;
		}
		while (jt+1 != ooSameLayerEnd &&
				(jt+1)->geomType == gTyp &&
				(jt+1)->attributes == jt->attributes) {
			jt++;
			try
			{
				MultiLinestring gNew = boost::get<MultiLinestring>(jt->buildWayGeometry(*sharedData->osmStore, &bbox, sharedData->cachedGeometries));
				MultiLinestring gTmp;
				geom::union_(*gAcc, gNew, gTmp);
				*gAcc = move(gTmp);
			}
			catch (std::out_of_range &err)
			{
				if (sharedData->verbose)
					cerr << "Error while processing LINESTRING " << jt->geomType << "," << jt->objectID <<"," << err.what() << endl;
			}
			catch (boost::bad_get &err) {
				cerr << "Error while processing LINESTRING " << jt->objectID << " has unexpected type" << endl;
				continue;
			}
		}
	}
}

void ProcessObjects(const OutputObjectsConstIt &ooSameLayerBegin, const OutputObjectsConstIt &ooSameLayerEnd, 
	class SharedData *sharedData, double simplifyLevel, TileBbox &bbox,
	vector_tile::Tile_Layer *vtLayer, vector<string> &keyList, vector<vector_tile::Tile_Value> &valueList)
{
	NodeStore &nodes = sharedData->osmStore->nodes;

	for (OutputObjectsConstIt jt = ooSameLayerBegin; jt != ooSameLayerEnd; ++jt) {
			
		if (jt->geomType == POINT) {
			vector_tile::Tile_Feature *featurePtr = vtLayer->add_features();
			jt->buildNodeGeometry(nodes.at(jt->objectID), &bbox, featurePtr);
			jt->writeAttributes(&keyList, &valueList, featurePtr);
			if (sharedData->includeID) { featurePtr->set_id(jt->objectID); }
		} else {
			Geometry g;
			try {
				g = jt->buildWayGeometry(*sharedData->osmStore, &bbox, sharedData->cachedGeometries);
			}
			catch (std::out_of_range &err)
			{
				if (sharedData->verbose)
					cerr << "Error while processing geometry " << jt->geomType << "," << jt->objectID <<"," << err.what() << endl;
				continue;
			}

			//This may increment the jt iterator
			CheckNextObjectAndMerge(jt, ooSameLayerEnd, sharedData, bbox, g);

			vector_tile::Tile_Feature *featurePtr = vtLayer->add_features();
			WriteGeometryVisitor w(&bbox, featurePtr, simplifyLevel);
			boost::apply_visitor(w, g);
			if (featurePtr->geometry_size()==0) { vtLayer->mutable_features()->RemoveLast(); continue; }
			jt->writeAttributes(&keyList, &valueList, featurePtr);
			if (sharedData->includeID) { featurePtr->set_id(jt->objectID); }

		}
	}
}

void ProcessLayer(uint zoom, uint index, const vector<OutputObject> &ooList, vector_tile::Tile &tile, 
	TileBbox &bbox, std::vector<uint> &ltx, class SharedData *sharedData)
{
	vector<string> keyList;
	vector<vector_tile::Tile_Value> valueList;
	vector_tile::Tile_Layer *vtLayer = tile.add_layers();

	//uint tileX = index >> 16;
	uint tileY = index & 65535;

	// Loop through sub-layers
	for (auto mt = ltx.begin(); mt != ltx.end(); ++mt) {
		uint layerNum = *mt;
		LayerDef ld = sharedData->layers[layerNum];
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

		// compare only by `layer`
		auto layerComp = [](const OutputObject &x, const OutputObject &y) -> bool { return x.layer < y.layer; };
		// We get the range within ooList, where the layer of each object is `layerNum`.
		// Note that ooList is sorted by a lexicographic order, `layer` being the most significant.
		OutputObjectsConstItPair ooListSameLayer = equal_range(ooList.begin(), ooList.end(), OutputObject(POINT, layerNum, 0), layerComp);
		// Loop through output objects
		ProcessObjects(ooListSameLayer.first, ooListSameLayer.second, sharedData, simplifyLevel, bbox, vtLayer, keyList, valueList);
	}

	// If there are any objects, then add tags
	if (vtLayer->features_size()>0) {
		vtLayer->set_name(sharedData->layers[ltx.at(0)].name);
		vtLayer->set_version(sharedData->mvtVersion);
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
	uint index = 0;
	uint zoom = sharedData->zoom;
	for (auto it = sharedData->tileIndexForZoom->begin(); it != sharedData->tileIndexForZoom->end(); ++it) {
		uint interval = 100;
		if (zoom<9) { interval=1; } else if (zoom<11) { interval=10; }
		if (threadId == 0 && (tc % interval) == 0) {
			cout << "Zoom level " << zoom << ", writing tile " << tc << " of " << sharedData->tileIndexForZoom->size() << "               \r";
			cout.flush();
		}
		if (tc++ % sharedData->threadNum != threadId) continue;

		// Create tile
		vector_tile::Tile tile;
		index = it->first;
		TileBbox bbox(index,zoom);
		const vector<OutputObject> &ooList = it->second;
		if (sharedData->clippingBoxFromJSON && (sharedData->maxLon<=bbox.minLon 
			|| sharedData->minLon>=bbox.maxLon || sharedData->maxLat<=bbox.minLat 
			|| sharedData->minLat>=bbox.maxLat)) { continue; }

		// Loop through layers
		for (auto lt = sharedData->layerOrder.begin(); lt != sharedData->layerOrder.end(); ++lt) {
			ProcessLayer(zoom, index, ooList, tile, bbox, *lt, sharedData);
		}

		// Write to file or sqlite

		string data, compressed;
		if (sharedData->sqlite) {
			// Write to sqlite
			tile.SerializeToString(&data);
			if (sharedData->compress) { compressed = compress_string(data, Z_DEFAULT_COMPRESSION, sharedData->gzip); }
			sharedData->mbtiles.saveTile(zoom, bbox.tilex, bbox.tiley, sharedData->compress ? &compressed : &data);

		} else {
			// Write to file
			stringstream dirname, filename;
			dirname  << sharedData->outputFile << "/" << zoom << "/" << bbox.tilex;
			filename << sharedData->outputFile << "/" << zoom << "/" << bbox.tilex << "/" << bbox.tiley << ".pbf";
			boost::filesystem::create_directories(dirname.str());
			fstream outfile(filename.str(), ios::out | ios::trunc | ios::binary);
			if (sharedData->compress) {
				tile.SerializeToString(&data);
				outfile << compress_string(data, Z_DEFAULT_COMPRESSION, sharedData->gzip);
			} else {
				if (!tile.SerializeToOstream(&outfile)) { cerr << "Couldn't write to " << filename.str() << endl; return -1; }
			}
			outfile.close();
		}
	}
	return 0;

}

