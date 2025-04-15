/*! \file */ 
#include "tile_worker.h"
#include <fstream>
#include <boost/filesystem.hpp>
#include <vtzero/builder.hpp>
#include <signal.h>
#include "helpers.h"
#include "visvalingam.h"
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
	unsigned simplifyAlgo,
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
			if (simplifyAlgo==LayerDef::VISVALINGAM) {
				tmp.push_back(simplifyVis(ls, simplifyLevel));
			} else {
				tmp.push_back(simplify(ls, simplifyLevel));
			}
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
	unsigned simplifyAlgo,
	const MultiPolygon& mp
) {
	MultiPolygon current = bbox.scaleGeometry(mp);
	if (simplifyLevel>0) {
		if (simplifyAlgo == LayerDef::VISVALINGAM) {
			current = simplifyVis(current, simplifyLevel/bbox.xscale);
		} else {
			current = simplify(current, simplifyLevel/bbox.xscale);
		}
		geom::remove_spikes(current);
	}
	if (geom::is_empty(current))
		return;

	geom::validity_failure_type failure;
	if (verbose && !geom::is_valid(current, failure)) { 
		cout << "output multipolygon has " << boost_validity_error(failure) << endl; 

		if (!geom::is_valid(mp, failure)) 
			cout << "input multipolygon has " << boost_validity_error(failure) << endl; 
		else
			cout << "input multipolygon valid" << endl;
	}

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
	unsigned simplifyAlgo,
	double filterArea,
	bool combinePolygons,
	bool combinePoints,
	unsigned zoom,
	const TileBbox &bbox,
	vtzero::layer_builder& vtLayer
) {

	for (auto jt = ooSameLayerBegin; jt != ooSameLayerEnd; ++jt) {
		OutputObjectID oo = *jt;
		if (zoom < oo.oo.minZoom) { continue; }

		if (oo.oo.geomType == POINT_) {
			// The very first point; below we check if there are more compatible points
			// so that we can write a multipoint instead of many point features

			std::vector<std::pair<int, int>> multipoint;

			LatpLon pos = source->buildNodeGeometry(jt->oo.objectID, bbox);
			pair<int,int> xy = bbox.scaleLatpLon(pos.latp/10000000.0, pos.lon/10000000.0);
			multipoint.push_back(xy);

			while (jt<(ooSameLayerEnd-1) && oo.oo.compatible((jt+1)->oo) && combinePoints) {
				jt++;
				LatpLon pos = source->buildNodeGeometry(jt->oo.objectID, bbox);
				pair<int,int> xy = bbox.scaleLatpLon(pos.latp/10000000.0, pos.lon/10000000.0);
				multipoint.push_back(xy);
			}

			vtzero::point_feature_builder fbuilder{vtLayer};
			if (sharedData.config.includeID && oo.id) fbuilder.set_id(oo.id);

			fbuilder.add_points(multipoint.size());

			if (verbose && multipoint.size() > 1)
				std::cout << "Merging " << multipoint.size() << " points into a multipoint" << std::endl;

			for (const auto &point : multipoint)
				fbuilder.set_point(point.first, point.second);

			oo.oo.writeAttributes(attributeStore, fbuilder, zoom);
			fbuilder.commit();

			oo = *jt;
		} else {
			Geometry g;
			try {
				g = source->buildWayGeometry(oo.oo.geomType, oo.oo.objectID, bbox);
			} catch (std::out_of_range &err) {
				if (verbose) cerr << "Error while processing geometry " << oo.oo.geomType << "," << static_cast<int>(oo.oo.objectID) <<"," << err.what() << endl;
				continue;
			}

			//This may increment the jt iterator
			if (oo.oo.geomType == LINESTRING_ && zoom < sharedData.config.combineBelow) {
				// Append successive linestrings, then reorder afterwards
				while (jt<(ooSameLayerEnd-1) && oo.oo.compatible((jt+1)->oo)) {
					jt++;
					MultiLinestring to_merge = boost::get<MultiLinestring>(source->buildWayGeometry(jt->oo.geomType, jt->oo.objectID, bbox));
					for (auto &ls : to_merge) boost::get<MultiLinestring>(g).emplace_back(ls);
				}
				MultiLinestring reordered;
				ReorderMultiLinestring(boost::get<MultiLinestring>(g), reordered);
				g = move(reordered);
				oo = *jt;

			} else if (oo.oo.geomType == POLYGON_ && combinePolygons) {
				// Append successive multipolygons, then union afterwards
				std::vector<MultiPolygon> mps;
				while (jt<(ooSameLayerEnd-1) && oo.oo.compatible((jt+1)->oo)) {
					jt++;
					mps.emplace_back( boost::get<MultiPolygon>(source->buildWayGeometry(jt->oo.geomType, jt->oo.objectID, bbox)) );
				}
				if (!mps.empty()) { 
					mps.emplace_back(boost::get<MultiPolygon>(g));
					union_many(mps); g = mps.front();
				}
				oo = *jt;
			}

			if (oo.oo.geomType == POLYGON_ && filterArea > 0.0) {
				RemovePartsBelowSize(boost::get<MultiPolygon>(g), filterArea);
				if (geom::is_empty(g)) continue;
			}

			if (oo.oo.geomType == LINESTRING_ || oo.oo.geomType == MULTILINESTRING_)
				writeMultiLinestring(attributeStore, sharedData, vtLayer, bbox, oo, zoom, simplifyLevel, simplifyAlgo, boost::get<MultiLinestring>(g));
			else if (oo.oo.geomType == POLYGON_)
				writeMultiPolygon(attributeStore, sharedData, vtLayer, bbox, oo, zoom, simplifyLevel, simplifyAlgo, boost::get<MultiPolygon>(g));
		}
	}
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
	vtzero::vector_tile existingTile,
	vtzero::tile_builder& tile, 
	const TileBbox& bbox,
	const std::vector<uint>& ltx,
	SharedData& sharedData
) {
	std::string layerName = sharedData.layers.layers[ltx.at(0)].name;
	vtzero::layer_builder vtLayer{tile, layerName, sharedData.config.mvtVersion, bbox.hires ? 8192u : 4096u};

	vtzero::layer existingLayer = existingTile.get_layer_by_name(layerName);
	if (existingLayer) {
		while (auto feature = existingLayer.next_feature()) {
			vtzero::geometry_feature_builder fb{vtLayer};
			if (feature.has_id())
				fb.set_id(feature.id());
			fb.set_geometry(feature.geometry());
			while (auto property = feature.next_property()) {
				fb.add_property(property.key(), property.value());
			}
			fb.commit();
		}
	}

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
				simplifyLevel, ld.simplifyAlgo,
				filterArea, zoom < ld.combinePolygonsBelow, ld.combinePoints, zoom, bbox, vtLayer);
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
	vtzero::tile_builder tile;

	TileBbox bbox(coordinates, zoom, sharedData.config.highResolution && zoom==sharedData.config.endZoom, zoom==sharedData.config.endZoom);
	if (sharedData.config.clippingBoxFromJSON && (
			sharedData.config.maxLon <= bbox.minLon ||
			sharedData.config.minLon >= bbox.maxLon ||
			sharedData.config.maxLat <= bbox.minLat ||
			sharedData.config.minLat >= bbox.maxLat))
		return;

	// Read existing tile if merging
	std::string rawExistingTile;
	if (sharedData.mergeSqlite) {
		sharedData.mbtiles.readTileAndUncompress(rawExistingTile, zoom, bbox.index.x, bbox.index.y, sharedData.config.compress, sharedData.config.gzip);
	}
	vtzero::vector_tile existingTile{rawExistingTile};

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
		ProcessLayer(sources, attributeStore, coordinates, zoom, data, existingTile, tile, bbox, *lt, sharedData);
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
			tile.serialize(outputdata);
			outfile << outputdata;
		}
		outfile.close();
	}
}
