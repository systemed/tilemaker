/*! \file */ 
#include "tile_worker.h"
#include <fstream>
#include <boost/filesystem.hpp>
#include "helpers.h"
#include "write_geometry.h"
using namespace std;
extern bool verbose;

typedef std::pair<double,double> xy_pair;

// Connect disconnected linestrings within a MultiLinestring
void ReorderMultiLinestring(MultiLinestring &input, MultiLinestring &output) {
	// create a map of the start/end points of each linestring
	// (we should be able to do std::map<Point,unsigned>, but that errors)
	std::map<xy_pair,unsigned> startPoints;
	std::map<xy_pair,unsigned> endPoints;
	for (unsigned i=0; i<input.size(); i++) {
		startPoints[xy_pair(input[i][0].x(),input[i][0].y())] = i;
		endPoints[xy_pair(input[i][input[i].size()-1].x(),input[i][input[i].size()-1].y())] = i;
	}

	// then for each linestring:
	// [skip if it's already been handled]
	// 1. create an output linestring from it
	// 2. look to see if there's another linestring which starts at our end point, or terminates at our start point
	// 3. if there is, then append it, remove from the map, and repeat from 2
	std::vector<bool> added(input.size(), false);
	for (unsigned i=0; i<input.size(); i++) {
		if (added[i]) continue;
		Linestring ls = std::move(input[i]);
		added[i] = true;
		while (true) {
			Point lastPoint = ls[ls.size()-1];
			auto foundStart = startPoints.find(xy_pair(lastPoint.x(),lastPoint.y()));
			if (foundStart != startPoints.end()) {
				unsigned idx = foundStart->second;
				if (!added[idx] && input[idx].size()+ls.size()<6000) {
					ls.insert(ls.end(), input[idx].begin()+1, input[idx].end());
					added[idx] = true;
					continue;
				}
			}

			Point firstPoint = ls[0];
			auto foundEnd = endPoints.find(xy_pair(firstPoint.x(),firstPoint.y()));
			if (foundEnd != endPoints.end()) {
				unsigned idx = foundEnd->second;
				if (!added[idx] && input[idx].size()+ls.size()<6000) {
					ls.insert(ls.begin(), input[idx].begin(), input[idx].end()-1);
					added[idx] = true;
					continue;
				}
			}

			break;
		}
		output.resize(output.size()+1);
		output[output.size()-1] = std::move(ls);
	}
}

template <typename T>
void CheckNextObjectAndMerge(OSMStore &osmStore, OutputObjectsConstIt &jt, OutputObjectsConstIt ooSameLayerEnd, 
	const TileBbox &bbox, T &g) {

	// If a object is a linestring/polygon that is followed by
	// other linestrings/polygons with the same attributes,
	// the following objects are merged into the first object, by taking union of geometries.
	OutputObjectRef oo = *jt;
	OutputObjectRef ooNext;
	if(jt+1 != ooSameLayerEnd) ooNext = *(jt+1);

	OutputGeometryType gt = oo->geomType;
	while (jt+1 != ooSameLayerEnd &&
			ooNext->geomType == gt &&
			ooNext->attributes == oo->attributes) {
		jt++;
		oo = *jt;
		if(jt+1 != ooSameLayerEnd) ooNext = *(jt+1);
		else ooNext.reset();

		try {
			T to_merge = boost::get<T>(buildWayGeometry(osmStore, *oo, bbox));
			T output;
			geom::union_(g, to_merge, output);
			g = move(output);
		} catch (std::out_of_range &err) { cerr << "Geometry out of range " << gt << ": " << oo->objectID <<"," << err.what() << endl;
		} catch (boost::bad_get &err) { cerr << "Type error while processing " << gt << ": " << oo->objectID << endl;
		} catch (geom::inconsistent_turns_exception &err) { cerr << "Inconsistent turns error while processing " << gt << ": " << oo->objectID << endl;
		}
	}
}

void ProcessObjects(OSMStore &osmStore, OutputObjectsConstIt ooSameLayerBegin, OutputObjectsConstIt ooSameLayerEnd, 
	class SharedData &sharedData, double simplifyLevel, double filterArea, bool combinePolygons, unsigned zoom, const TileBbox &bbox,
	vector_tile::Tile_Layer *vtLayer, vector<string> &keyList, vector<vector_tile::Tile_Value> &valueList) {

	for (auto jt = ooSameLayerBegin; jt != ooSameLayerEnd; ++jt) {
		OutputObjectRef oo = *jt;
		if (zoom < oo->minZoom) { continue; }

		if (oo->geomType == OutputGeometryType::POINT) {
			vector_tile::Tile_Feature *featurePtr = vtLayer->add_features();
			LatpLon pos = buildNodeGeometry(osmStore, *oo, bbox);
			featurePtr->add_geometry(9);					// moveTo, repeat x1
			pair<int,int> xy = bbox.scaleLatpLon(pos.latp/10000000.0, pos.lon/10000000.0);
			featurePtr->add_geometry((xy.first  << 1) ^ (xy.first  >> 31));
			featurePtr->add_geometry((xy.second << 1) ^ (xy.second >> 31));
			featurePtr->set_type(vector_tile::Tile_GeomType_POINT);

			oo->writeAttributes(&keyList, &valueList, featurePtr, zoom);
			if (sharedData.config.includeID) { featurePtr->set_id(oo->objectID); }
		} else {
			Geometry g;
			try {
				g = buildWayGeometry(osmStore, *oo, bbox);
			} catch (std::out_of_range &err) {
				if (verbose) cerr << "Error while processing geometry " << oo->geomType << "," << oo->objectID <<"," << err.what() << endl;
				continue;
			}

			if (oo->geomType == OutputGeometryType::POLYGON && filterArea > 0.0) {
				if (geom::area(g)<filterArea) continue;
			}

			//This may increment the jt iterator
			if (oo->geomType == OutputGeometryType::LINESTRING && zoom < sharedData.config.combineBelow) {
				CheckNextObjectAndMerge(osmStore, jt, ooSameLayerEnd, bbox, boost::get<MultiLinestring>(g));
				MultiLinestring reordered;
				ReorderMultiLinestring(boost::get<MultiLinestring>(g), reordered);
				g = move(reordered);
				oo = *jt;
			} else if (oo->geomType == OutputGeometryType::POLYGON && combinePolygons) {
				CheckNextObjectAndMerge(osmStore, jt, ooSameLayerEnd, bbox, boost::get<MultiPolygon>(g));
				oo = *jt;
			}

			vector_tile::Tile_Feature *featurePtr = vtLayer->add_features();
			WriteGeometryVisitor w(&bbox, featurePtr, simplifyLevel);
			boost::apply_visitor(w, g);
			if (featurePtr->geometry_size()==0) { vtLayer->mutable_features()->RemoveLast(); continue; }
			oo->writeAttributes(&keyList, &valueList, featurePtr, zoom);
			if (sharedData.config.includeID) { featurePtr->set_id(oo->objectID); }

		}
	}
}

vector_tile::Tile_Layer* findLayerByName(vector_tile::Tile &tile, std::string &layerName, vector<string> &keyList, vector<vector_tile::Tile_Value> &valueList) {
	for (unsigned i=0; i<tile.layers_size(); i++) {
		if (tile.layers(i).name()!=layerName) continue;
		// we already have this layer, so copy the key/value lists, and return it
		for (unsigned j=0; j<tile.layers(i).keys_size(); j++) keyList.emplace_back(tile.layers(i).keys(j));
		for (unsigned j=0; j<tile.layers(i).values_size(); j++) valueList.emplace_back(tile.layers(i).values(j));
		return tile.mutable_layers(i);
	}
	// not found, so add new layer
	return tile.add_layers();
}

void ProcessLayer(OSMStore &osmStore,
    TileCoordinates index, uint zoom, std::vector<OutputObjectRef> const &data, vector_tile::Tile &tile, 
	const TileBbox &bbox, const std::vector<uint> &ltx, SharedData &sharedData)
{
	vector<string> keyList;
	vector<vector_tile::Tile_Value> valueList;
	std::string layerName = sharedData.layers.layers[ltx.at(0)].name;
	vector_tile::Tile_Layer *vtLayer = sharedData.mergeSqlite ? findLayerByName(tile, layerName, keyList, valueList) : tile.add_layers();

	//TileCoordinate tileX = index.x;
	TileCoordinate tileY = index.y;

	// Loop through sub-layers
	for (auto mt = ltx.begin(); mt != ltx.end(); ++mt) {
		uint layerNum = *mt;
		const LayerDef &ld = sharedData.layers.layers[layerNum];
		if (zoom<ld.minzoom || zoom>ld.maxzoom) { continue; }
		double simplifyLevel = 0.0, filterArea = 0.0, latp = 0.0;
		if (zoom < ld.simplifyBelow || zoom < ld.filterBelow) {
			latp = (tiley2latp(tileY, zoom) + tiley2latp(tileY+1, zoom)) / 2;
		}
		if (zoom < ld.simplifyBelow) {
			if (ld.simplifyLength > 0) {
				simplifyLevel = meter2degp(ld.simplifyLength, latp);
			} else {
				simplifyLevel = ld.simplifyLevel;
			}
			simplifyLevel *= pow(ld.simplifyRatio, (ld.simplifyBelow-1) - zoom);
		}
		if (zoom < ld.filterBelow) { 
			filterArea = meter2degp(ld.filterArea, latp) * pow(2.0, (ld.filterBelow-1) - zoom);
		}

		auto ooListSameLayer = GetObjectsAtSubLayer(data, layerNum);
		// Loop through output objects
		ProcessObjects(osmStore, ooListSameLayer.first, ooListSameLayer.second, sharedData, 
			simplifyLevel, filterArea, zoom < ld.combinePolygonsBelow, zoom, bbox, vtLayer, keyList, valueList);
	}

	// If there are any objects, then add tags
	if (vtLayer->features_size()>0) {
		vtLayer->set_name(layerName);
		vtLayer->set_version(sharedData.config.mvtVersion);
		vtLayer->set_extent(4096);
		for (uint j=vtLayer->keys_size(); j<keyList.size(); j++) {
			vtLayer->add_keys(keyList[j]);
		}
		for (uint j=vtLayer->values_size(); j<valueList.size(); j++) { 
			vector_tile::Tile_Value *v = vtLayer->add_values();
			*v = valueList[j];
		}
	} else {
		tile.mutable_layers()->RemoveLast();
	}
}

bool outputProc(boost::asio::thread_pool &pool, SharedData &sharedData, OSMStore &osmStore, std::vector<OutputObjectRef> const &data, TileCoordinates coordinates, uint zoom)
{
	// Create tile
	vector_tile::Tile tile;
	TileBbox bbox(coordinates, zoom);
	if (sharedData.config.clippingBoxFromJSON && (sharedData.config.maxLon<=bbox.minLon 
		|| sharedData.config.minLon>=bbox.maxLon || sharedData.config.maxLat<=bbox.minLat 
		|| sharedData.config.minLat>=bbox.maxLat)) { return true; }

	// Read existing tile if merging
	if (sharedData.mergeSqlite) {
		std::string rawTile;
		if (sharedData.mbtiles.readTileAndUncompress(rawTile, zoom, bbox.index.x, bbox.index.y, sharedData.config.compress, sharedData.config.gzip)) {
			tile.ParseFromString(rawTile);
		}
	}

	// Loop through layers
	for (auto lt = sharedData.layers.layerOrder.begin(); lt != sharedData.layers.layerOrder.end(); ++lt) {
		ProcessLayer(osmStore, coordinates, zoom, data, tile, bbox, *lt, sharedData);
	}

	// Write to file or sqlite
	string outputdata, compressed;
	if (sharedData.sqlite) {
		// Write to sqlite
		tile.SerializeToString(&outputdata);
		if (sharedData.config.compress) { compressed = compress_string(outputdata, Z_DEFAULT_COMPRESSION, sharedData.config.gzip); }
		sharedData.mbtiles.saveTile(zoom, bbox.index.x, bbox.index.y, sharedData.config.compress ? &compressed : &outputdata);

	} else {
		// Write to file
		stringstream dirname, filename;
		dirname  << sharedData.outputFile << "/" << zoom << "/" << bbox.index.x;
		filename << sharedData.outputFile << "/" << zoom << "/" << bbox.index.x << "/" << bbox.index.y << ".pbf";
		boost::filesystem::create_directories(dirname.str());
		fstream outfile(filename.str(), ios::out | ios::trunc | ios::binary);
		if (sharedData.config.compress) {
			tile.SerializeToString(&outputdata);
			outfile << compress_string(outputdata, Z_DEFAULT_COMPRESSION, sharedData.config.gzip);
		} else {
			if (!tile.SerializeToOstream(&outfile)) { cerr << "Couldn't write to " << filename.str() << endl; return false; }
		}
		outfile.close();
	}

	return true;
}

