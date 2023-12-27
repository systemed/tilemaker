/*! \file */ 
#include "tile_worker.h"
#include <fstream>
#include <boost/filesystem.hpp>
#include <vtzero/builder.hpp>
#include <signal.h>
#include "helpers.h"
using namespace std;
extern bool verbose;

thread_local bool enabledUserSignal = false;
typedef std::vector<OutputObjectID>::const_iterator OutputObjectsConstIt;
typedef std::pair<OutputObjectsConstIt, OutputObjectsConstIt> OutputObjectsConstItPair;

typedef std::pair<double,double> xy_pair;
namespace std {
	template<>
	struct hash<xy_pair> {
		size_t operator()(const xy_pair &xy) const {
			return std::hash<double>()(xy.first) ^ std::hash<double>()(xy.second);
		}
	};
}

// Connect disconnected linestrings within a MultiLinestring
void ReorderMultiLinestring(MultiLinestring &input, MultiLinestring &output) {
	// create a map of the start/end points of each linestring
	// (we should be able to do std::map<Point,unsigned>, but that errors)
	std::unordered_map<xy_pair,unsigned> startPoints;
	std::unordered_map<xy_pair,unsigned> endPoints;
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

// Merge two multilinestrings by simply appending
// (the constituent parts will be matched up in subsequent call to ReorderMultiLinestring)
void MergeIntersecting(MultiLinestring &input, MultiLinestring &to_merge) {
	for (auto ls : to_merge) input.emplace_back(ls);
}

// Merge two multipolygons by doing intersection checks for each constituent polygon
void MergeIntersecting(MultiPolygon &input, MultiPolygon &to_merge) {
	if (boost::geometry::intersects(input, to_merge)) {
		for (std::size_t i=0; i<input.size(); i++) {
			if (boost::geometry::intersects(input[i], to_merge)) {
				MultiPolygon union_result;
				boost::geometry::union_(input[i], to_merge, union_result);
				for (auto output : union_result) input.emplace_back(output);
				input.erase(input.begin() + i);
				return;
			}
		}
	}
	for (auto output : to_merge) input.emplace_back(output);
}

template <typename T>
void CheckNextObjectAndMerge(
	TileDataSource* source,
	OutputObjectsConstIt& jt,
	OutputObjectsConstIt ooSameLayerEnd, 
	const TileBbox& bbox,
	T& g
) {
	if (jt + 1 == ooSameLayerEnd)
		return;

	// If an object is a linestring/polygon that is followed by
	// other linestrings/polygons with the same attributes,
	// the following objects are merged into the first object, by taking union of geometries.
	OutputObjectID oo = *jt;
	OutputObjectID ooNext = *(jt + 1);

	// TODO: do we need ooNext? Could we instead just update jt and dereference it?
	//       put differently: we don't need to keep overwriting oo/ooNext
	OutputGeometryType gt = oo.oo.geomType;
	while (jt + 1 != ooSameLayerEnd &&
			ooNext.oo.geomType == gt &&
			ooNext.oo.z_order == oo.oo.z_order &&
			ooNext.oo.attributes == oo.oo.attributes) {
		jt++;
		oo = *jt;
		if(jt + 1 != ooSameLayerEnd) {
			ooNext = *(jt + 1);
		}

		try {
			T to_merge = boost::get<T>(source->buildWayGeometry(oo.oo.geomType, oo.oo.objectID, bbox));
			MergeIntersecting(g, to_merge);
		} catch (std::out_of_range &err) { cerr << "Geometry out of range " << gt << ": " << static_cast<int>(oo.oo.objectID) <<"," << err.what() << endl;
		} catch (boost::bad_get &err) { cerr << "Type error while processing " << gt << ": " << static_cast<int>(oo.oo.objectID) << endl;
		} catch (geom::inconsistent_turns_exception &err) { cerr << "Inconsistent turns error while processing " << gt << ": " << static_cast<int>(oo.oo.objectID) << endl;
		}
	}
}

void RemovePartsBelowSize(MultiPolygon &g, double filterArea) {
	g.erase(std::remove_if(
		g.begin(),
		g.end(),
		[&](const Polygon &poly) -> bool {
			return std::fabs(geom::area(poly)) < filterArea;
		}),
	g.end());
	for (auto &outer : g) {
		outer.inners().erase(std::remove_if(
			outer.inners().begin(), 
			outer.inners().end(), 
			[&](const Ring &inner) -> bool { 
				return std::fabs(geom::area(inner)) < filterArea;
			}),
		outer.inners().end());
	}
}

void writeMultiLinestring(
	const AttributeStore& attributeStore,
	const SharedData& sharedData,
	vtzero::layer_builder& vtLayer,
	const TileBbox& bbox,
	const OutputObjectID& oo,
	unsigned zoom,
	double simplifyLevel,
	const MultiLinestring& mls
) {
	vtzero::linestring_feature_builder fbuilder{vtLayer};

	if (sharedData.config.includeID && oo.id)
		fbuilder.set_id(oo.id);

	bool hadLine = false;

	MultiLinestring tmp;
	const MultiLinestring* toWrite = nullptr;

	if (simplifyLevel>0) {
		for(auto const &ls: mls) {
			tmp.push_back(simplify(ls, simplifyLevel));
		}
		toWrite = &tmp;
	} else {
		toWrite = &mls;
	}

	for (const Linestring& ls : *toWrite) {
		if (ls.size() <= 1)
			continue;

		pair<int, int> lastXy = std::make_pair(0, 0);

		// vtzero dislikes linesegments that have zero-length segments,
		// e.g. where p(x) == p(x + 1). So filter those out.
		int points = 0;
		for (const Point& p : ls) {
			pair<int,int> xy = bbox.scaleLatpLon(p.get<1>(), p.get<0>());
			if (points == 0 || xy != lastXy) {
				points++;
				lastXy = xy;
			}
		}

		// A line has at least 2 points.
		if (points <= 1)
			continue;

		hadLine = true;
		fbuilder.add_linestring(points);
		bool firstPoint = true;
		for (const Point& p : ls) {
			pair<int,int> xy = bbox.scaleLatpLon(p.get<1>(), p.get<0>());
			if (firstPoint || xy != lastXy) {
				// vtzero doesn't like linesegments with zero-length segments,
				// so filter those out
				fbuilder.set_point(xy.first, xy.second);
				firstPoint = false;
				lastXy = xy;
			}
		}
	}

	if (hadLine) {
		// add the properties
		oo.oo.writeAttributes(attributeStore, fbuilder, zoom);
		// call commit() when you are done
		fbuilder.commit();
	}
}

bool writeRing(
	vtzero::polygon_feature_builder& fbuilder,
	const Ring& ring
) {
	// vtzero doesn't like zero-length segments in rings
	pair<int, int> lastXy = std::make_pair(0, 0);
	int points = 0;
	for (const Point& point : ring) {
		pair<int, int> xy = std::make_pair(point.get<0>(), point.get<1>());
		if (points == 0 || xy != lastXy) {
			points++;
			lastXy = xy;
		}
	}

	// A ring has at least 4 points.
	if (points <= 3)
		return false;

	bool firstPoint = true;
	fbuilder.add_ring(points);
	for (const Point& point : ring) {
		pair<int, int> xy = std::make_pair(point.get<0>(), point.get<1>());

		if (firstPoint || xy != lastXy) {
			firstPoint = false;
			lastXy = xy;
			fbuilder.set_point(xy.first, xy.second);
		}
	}

	return true;
}


void writeMultiPolygon(
	const AttributeStore& attributeStore,
	const SharedData& sharedData,
	vtzero::layer_builder& vtLayer,
	const TileBbox& bbox,
	const OutputObjectID& oo,
	unsigned zoom,
	double simplifyLevel,
	const MultiPolygon& mp
) {
	MultiPolygon current = bbox.scaleGeometry(mp);
	if (simplifyLevel>0) {
		current = simplify(current, simplifyLevel/bbox.xscale);
		geom::remove_spikes(current);
	}
	if (geom::is_empty(current))
		return;

#if BOOST_VERSION >= 105800
	geom::validity_failure_type failure;
	if (verbose && !geom::is_valid(current, failure)) { 
		cout << "output multipolygon has " << boost_validity_error(failure) << endl; 

		if (!geom::is_valid(mp, failure)) 
			cout << "input multipolygon has " << boost_validity_error(failure) << endl; 
		else
			cout << "input multipolygon valid" << endl;
	}
#else	
	if (verbose && !geom::is_valid(current)) { 
		cout << "Output multipolygon is invalid " << endl; 
	}
#endif

	vtzero::polygon_feature_builder fbuilder{vtLayer};

	if (sharedData.config.includeID && oo.id)
		fbuilder.set_id(oo.id);

	bool hadPoly = false;

	for (const auto& p : current) {
		const Ring& ring = geom::exterior_ring(p);

		// If we failed to write the outer, no need to write the inners.
		if (!writeRing(fbuilder, ring))
			continue;

		hadPoly = true;

		const InteriorRing& interiors = geom::interior_rings(p);
		for (const Ring& ring : interiors)
			writeRing(fbuilder, ring);
	}

	if (hadPoly) {
		// add the properties
		oo.oo.writeAttributes(attributeStore, fbuilder, zoom);
		// call commit() when you are done
		fbuilder.commit();
	}
}

void ProcessObjects(
	TileDataSource* source,
	const AttributeStore& attributeStore,
	OutputObjectsConstIt ooSameLayerBegin,
	OutputObjectsConstIt ooSameLayerEnd, 
	class SharedData& sharedData,
	double simplifyLevel,
	double filterArea,
	bool combinePolygons,
	unsigned zoom,
	const TileBbox &bbox,
	vtzero::layer_builder& vtLayer,
	vector<string>& keyList,
	vector<vector_tile::Tile_Value>& valueList
) {

	for (auto jt = ooSameLayerBegin; jt != ooSameLayerEnd; ++jt) {
		OutputObjectID oo = *jt;
		if (zoom < oo.oo.minZoom) { continue; }

		if (oo.oo.geomType == POINT_) {
			vtzero::point_feature_builder fbuilder{vtLayer};
			if (sharedData.config.includeID && oo.id) fbuilder.set_id(oo.id);

			LatpLon pos = source->buildNodeGeometry(oo.oo.objectID, bbox);
			pair<int,int> xy = bbox.scaleLatpLon(pos.latp/10000000.0, pos.lon/10000000.0);
			fbuilder.add_point(xy.first, xy.second);
			oo.oo.writeAttributes(attributeStore, fbuilder, zoom);
			fbuilder.commit();
		} else {
			Geometry g;
			try {
				g = source->buildWayGeometry(oo.oo.geomType, oo.oo.objectID, bbox);
			} catch (std::out_of_range &err) {
				if (verbose) cerr << "Error while processing geometry " << oo.oo.geomType << "," << static_cast<int>(oo.oo.objectID) <<"," << err.what() << endl;
				continue;
			}

			if (oo.oo.geomType == POLYGON_ && filterArea > 0.0) {
				RemovePartsBelowSize(boost::get<MultiPolygon>(g), filterArea);
				if (geom::is_empty(g)) continue;
			}

			//This may increment the jt iterator
			if (oo.oo.geomType == LINESTRING_ && zoom < sharedData.config.combineBelow) {
				CheckNextObjectAndMerge(source, jt, ooSameLayerEnd, bbox, boost::get<MultiLinestring>(g));
				MultiLinestring reordered;
				ReorderMultiLinestring(boost::get<MultiLinestring>(g), reordered);
				g = move(reordered);
				oo = *jt;
			} else if (oo.oo.geomType == POLYGON_ && combinePolygons) {
				CheckNextObjectAndMerge(source, jt, ooSameLayerEnd, bbox, boost::get<MultiPolygon>(g));
				oo = *jt;
			}

			if (oo.oo.geomType == LINESTRING_ || oo.oo.geomType == MULTILINESTRING_)
				writeMultiLinestring(attributeStore, sharedData, vtLayer, bbox, oo, zoom, simplifyLevel, boost::get<MultiLinestring>(g));
			else if (oo.oo.geomType == POLYGON_)
				writeMultiPolygon(attributeStore, sharedData, vtLayer, bbox, oo, zoom, simplifyLevel, boost::get<MultiPolygon>(g));
		}
	}
}

vector_tile::Tile_Layer* findLayerByName(
	vector_tile::Tile& tile,
	std::string& layerName,
	vector<string>& keyList,
	vector<vector_tile::Tile_Value>& valueList
) {
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

OutputObjectsConstItPair getObjectsAtSubLayer(
	const std::vector<OutputObjectID>& data,
	uint_least8_t layerNum
) {
    struct layerComp
    {
        bool operator() ( const OutputObjectID& x, uint_least8_t layer ) const { return x.oo.layer < layer; }
        bool operator() ( uint_least8_t layer, const OutputObjectID& x ) const { return layer < x.oo.layer; }
    };

	// compare only by `layer`
	// We get the range within ooList, where the layer of each object is `layerNum`.
	// Note that ooList is sorted by a lexicographic order, `layer` being the most significant.
	return equal_range(data.begin(), data.end(), layerNum, layerComp());
}



void ProcessLayer(
	const SourceList& sources,
	const AttributeStore& attributeStore,
	TileCoordinates index,
	uint zoom, 
	const std::vector<std::vector<OutputObjectID>>& data,
	vtzero::tile_builder& tile, 
	const TileBbox& bbox,
	const std::vector<uint>& ltx,
	SharedData& sharedData
) {
	vector<string> keyList;
	vector<vector_tile::Tile_Value> valueList;
	std::string layerName = sharedData.layers.layers[ltx.at(0)].name;
	// TODO: revive mergeSqlite
//	vector_tile::Tile_Layer *vtLayer = sharedData.mergeSqlite ? findLayerByName(tile, layerName, keyList, valueList) : tile.add_layers();
	vtzero::layer_builder vtLayer{tile, layerName, sharedData.config.mvtVersion, bbox.hires ? 8192u : 4096u};


	//TileCoordinate tileX = index.x;
	TileCoordinate tileY = index.y;

	// Loop through sub-layers
	std::time_t start = std::time(0);
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

		for (size_t i=0; i<sources.size(); i++) {
			// Loop through output objects
			auto ooListSameLayer = getObjectsAtSubLayer(data[i], layerNum);
			auto end = ooListSameLayer.second;
			if (ld.featureLimit>0 && end-ooListSameLayer.first>ld.featureLimit && zoom<ld.featureLimitBelow) end = ooListSameLayer.first+ld.featureLimit;
			ProcessObjects(sources[i], attributeStore, 
				ooListSameLayer.first, end, sharedData, 
				simplifyLevel, filterArea, zoom < ld.combinePolygonsBelow, zoom, bbox, vtLayer, keyList, valueList);
		}
	}
	if (verbose && std::time(0)-start>3) {
		std::cout << "Layer " << layerName << " at " << zoom << "/" << index.x << "/" << index.y << " took " << (std::time(0)-start) << " seconds" << std::endl;
	}
}

bool signalStop=false;
void handleUserSignal(int signum) {
	std::cout << "User requested break in processing" << std::endl;
	signalStop=true;
}

void outputProc(
	SharedData& sharedData, 
	const SourceList& sources,
	const AttributeStore& attributeStore,
	const std::vector<std::vector<OutputObjectID>>& data, 
	TileCoordinates coordinates,
	uint zoom
) {
	// Create tile
	// vector_tile::Tile tile;
	vtzero::tile_builder tile;

	TileBbox bbox(coordinates, zoom, sharedData.config.highResolution && zoom==sharedData.config.endZoom, zoom==sharedData.config.endZoom);
	if (sharedData.config.clippingBoxFromJSON && (
			sharedData.config.maxLon <= bbox.minLon ||
			sharedData.config.minLon >= bbox.maxLon ||
			sharedData.config.maxLat <= bbox.minLat ||
			sharedData.config.minLat >= bbox.maxLat))
		return;

	// Read existing tile if merging
	// TODO: revive mergeSqlite
	/*
	if (sharedData.mergeSqlite) {
		std::string rawTile;
		if (sharedData.mbtiles.readTileAndUncompress(rawTile, zoom, bbox.index.x, bbox.index.y, sharedData.config.compress, sharedData.config.gzip)) {
			tile.ParseFromString(rawTile);
		}
	}
	*/

	// Loop through layers
#ifndef _WIN32
	if (!enabledUserSignal) {
		signal(SIGUSR1, handleUserSignal);
		enabledUserSignal = true;
	}
#endif
	signalStop=false;

	for (auto lt = sharedData.layers.layerOrder.begin(); lt != sharedData.layers.layerOrder.end(); ++lt) {
		if (signalStop) break;
		ProcessLayer(sources, attributeStore, coordinates, zoom, data, tile, bbox, *lt, sharedData);
	}

	// Write to file or sqlite
	string outputdata, compressed;
	if (sharedData.outputMode == OptionsParser::OutputMode::MBTiles) {
		// Write to sqlite
		//tile.SerializeToString(&outputdata);
		tile.serialize(outputdata);

		if (sharedData.config.compress) { compressed = compress_string(outputdata, Z_DEFAULT_COMPRESSION, sharedData.config.gzip); }
		sharedData.mbtiles.saveTile(zoom, bbox.index.x, bbox.index.y, sharedData.config.compress ? &compressed : &outputdata, sharedData.mergeSqlite);

	} else if (sharedData.outputMode == OptionsParser::OutputMode::PMTiles) {
		// Write to pmtiles
		tile.serialize(outputdata);
		//tile.SerializeToString(&outputdata);
		sharedData.pmtiles.saveTile(zoom, bbox.index.x, bbox.index.y, outputdata);

	} else {
		// Write to file
		stringstream dirname, filename;
		dirname  << sharedData.outputFile << "/" << zoom << "/" << bbox.index.x;
		filename << sharedData.outputFile << "/" << zoom << "/" << bbox.index.x << "/" << bbox.index.y << ".pbf";
		boost::filesystem::create_directories(dirname.str());
		fstream outfile(filename.str(), ios::out | ios::trunc | ios::binary);
		if (sharedData.config.compress) {
			//tile.SerializeToString(&outputdata);
			tile.serialize(outputdata);
			outfile << compress_string(outputdata, Z_DEFAULT_COMPRESSION, sharedData.config.gzip);
		} else {
			// TODO: verify this works
			tile.serialize(outputdata);
			outfile << outputdata;

			/*
			if (!tile.SerializeToOstream(&outfile)) {
				cerr << "Couldn't write to " << filename.str() << endl;
			}
			*/
		}
		outfile.close();
	}
}
