/*
	tilemaker
	Richard Fairhurst, June 2015
*/

// C++ includes
#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <cmath>
#include <stdexcept>
#include <thread>
#include <mutex>

// Other utilities
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <boost/variant.hpp>
#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/filereadstream.h"
#include "sqlite_modern_cpp.h"

#ifdef _MSC_VER
typedef unsigned uint;
#endif
// Protobuf
#include "osmformat.pb.h"
#include "vector_tile.pb.h"

// Shapelib
#include "shapefil.h"

// Lua
extern "C" {
	#include "lua.h"
    #include "lualib.h"
    #include "lauxlib.h"
}
#include "kaguya.hpp"

// boost::geometry
#include <boost/geometry.hpp>
#include <boost/geometry/algorithms/intersection.hpp>
#include <boost/geometry/geometries/geometries.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/index/rtree.hpp>
typedef boost::geometry::model::d2::point_xy<double> Point; 
typedef boost::geometry::model::linestring<Point> Linestring;
typedef boost::geometry::model::polygon<Point> Polygon;
typedef boost::geometry::model::multi_polygon<Polygon> MultiPolygon;
typedef boost::geometry::model::multi_linestring<Linestring> MultiLinestring;
typedef boost::geometry::model::box<Point> Box;
typedef boost::geometry::ring_type<Polygon>::type Ring;
typedef boost::geometry::interior_type<Polygon>::type InteriorRing;
typedef boost::variant<Point,Linestring,MultiLinestring,MultiPolygon> Geometry;
typedef std::pair<Box, uint> IndexValue;
typedef boost::geometry::index::rtree< IndexValue, boost::geometry::index::quadratic<16> > RTree;

// Namespaces
using namespace std;
using namespace sqlite;
namespace po = boost::program_options;
namespace geom = boost::geometry;

// Tilemaker code
#include "helpers.cpp"
#include "pbf_blocks.cpp"
#include "coordinates.cpp"

#ifdef COMPACT_NODES
typedef uint32_t NodeID;
#else
typedef uint64_t NodeID;
#endif
typedef uint32_t WayID;
#define MAX_WAY_ID 4294967295
typedef vector<NodeID> NodeVec;
typedef vector<WayID> WayVec;

#include "osm_store.cpp"
#include "output_object.cpp"
#include "osm_object.cpp"
#include "mbtiles.cpp"
#include "read_shp.cpp"
#include "write_geometry.cpp"

kaguya::State luaState;

int lua_error_handler(int errCode, const char *errMessage)
{
	std::string traceback = luaState["debug"]["traceback"];
	cerr << "lua runtime error: " << errMessage << endl;
	cerr << "traceback: " << traceback << endl;
	exit(0);
}

int main(int argc, char* argv[]) {

	// ----	Initialise data collections

	OSMStore osmStore;									// global OSM store
	NodeStore &nodes = osmStore.nodes;
	WayStore &ways = osmStore.ways;
	RelationStore &relations = osmStore.relations;

	map<string, RTree> indices;						// boost::geometry::index objects for shapefile indices
	vector<Geometry> cachedGeometries;					// prepared boost::geometry objects (from shapefiles)
	map<uint, string> cachedGeometryNames;			//  | optional names for each one

	map< uint, vector<OutputObject> > tileIndex;				// objects to be output

	// ----	Read command-line options
	
	bool sqlite=false;
	vector<string> inputFiles;
	string outputFile;
	string luaFile;
	string jsonFile;
	bool verbose = false;
	uint threadNum;

	po::options_description desc("tilemaker (c) 2016 Richard Fairhurst and contributors\nConvert OpenStreetMap .pbf files into vector tiles\n\nAvailable options");
	desc.add_options()
		("help",                                                                 "show help message")
		("input",  po::value< vector<string> >(&inputFiles),                     "source .osm.pbf file")
		("output", po::value< string >(&outputFile),                             "target directory or .mbtiles/.sqlite file")
		("config", po::value< string >(&jsonFile)->default_value("config.json"), "config JSON file")
		("process",po::value< string >(&luaFile)->default_value("process.lua"),  "tag-processing Lua file")
		("verbose",po::bool_switch(&verbose),                                    "verbose error output")
		("threads",po::value< uint >(&threadNum)->default_value(0),              "number of threads (automatically detected if 0)");
	po::positional_options_description p;
	p.add("input", -1);
	po::variables_map vm;
	po::store(po::command_line_parser(argc, argv).options(desc).positional(p).run(), vm);
	po::notify(vm);
	
	if (vm.count("help")) { cout << desc << endl; return 1; }
	if (vm.count("output")==0) { cerr << "You must specify an output file or directory. Run with --help to find out more." << endl; return -1; }
	if (vm.count("input")==0) { cerr << "You must specify at least one source .osm.pbf file. Run with --help to find out more." << endl; return -1; }

	if (ends_with(outputFile, ".mbtiles") || ends_with(outputFile, ".sqlite")) {
		sqlite=true;
	}
	if (threadNum == 0) {
		threadNum = max(thread::hardware_concurrency(), 1u);
	}

	#ifdef COMPACT_NODES
	cout << "tilemaker compiled without 64-bit node support, use 'osmium renumber' first if working with OpenStreetMap-sourced data" << endl;
	#endif

	// ----	Read bounding box from first .pbf

	Box clippingBox;
	bool hasClippingBox = false;
	bool clippingBoxFromJSON = false;
	fstream infile(inputFiles[0], ios::in | ios::binary);
	if (!infile) { cerr << "Couldn't open .pbf file " << inputFiles[0] << endl; return -1; }
	HeaderBlock block;
	readBlock(&block, &infile);
	if (block.has_bbox()) {
		hasClippingBox = true;
		double minLon = block.bbox().left()  /1000000000.0;
		double maxLon = block.bbox().right() /1000000000.0;
		double minLat = block.bbox().bottom()/1000000000.0;
		double maxLat = block.bbox().top()   /1000000000.0;
		clippingBox = Box(geom::make<Point>(minLon, lat2latp(minLat)),
			              geom::make<Point>(maxLon, lat2latp(maxLat)));
	}
	infile.close();
	
	// ----	Initialise Lua

	luaState.setErrorHandler(lua_error_handler);
	luaState.dofile(luaFile.c_str());
	luaState["OSM"].setClass(kaguya::UserdataMetatable<OSMObject>()
		.addFunction("Id", &OSMObject::Id)
		.addFunction("Holds", &OSMObject::Holds)
		.addFunction("Find", &OSMObject::Find)
		.addFunction("FindIntersecting", &OSMObject::FindIntersecting)
		.addFunction("Intersects", &OSMObject::Intersects)
		.addFunction("IsClosed", &OSMObject::IsClosed)
		.addFunction("ScaleToMeter", &OSMObject::ScaleToMeter)
		.addFunction("ScaleToKiloMeter", &OSMObject::ScaleToKiloMeter)
		.addFunction("Area", &OSMObject::Area)
		.addFunction("Length", &OSMObject::Length)
		.addFunction("Layer", &OSMObject::Layer)
		.addFunction("LayerAsCentroid", &OSMObject::LayerAsCentroid)
		.addFunction("Attribute", &OSMObject::Attribute)
		.addFunction("AttributeNumeric", &OSMObject::AttributeNumeric)
		.addFunction("AttributeBoolean", &OSMObject::AttributeBoolean)
	);
	OSMObject osmObject(&luaState, &indices, &cachedGeometries, &cachedGeometryNames, &osmStore);

	// ----	Read JSON config

	uint baseZoom, startZoom, endZoom;
	string projectName, projectVersion, projectDesc;
	bool includeID = false, compress = true, gzip = true;
	string compressOpt;
	rapidjson::Document jsonConfig;
	double minLon, minLat, maxLon, maxLat;
	try {
		FILE* fp = fopen(jsonFile.c_str(), "r");
		char readBuffer[65536];
		rapidjson::FileReadStream is(fp, readBuffer, sizeof(readBuffer));
		jsonConfig.ParseStream(is);
		if (jsonConfig.HasParseError()) { cerr << "Invalid JSON file." << endl; return -1; }
		fclose(fp);

		// Global config
		baseZoom       = jsonConfig["settings"]["basezoom"].GetUint();
		startZoom      = jsonConfig["settings"]["minzoom" ].GetUint();
		endZoom        = jsonConfig["settings"]["maxzoom" ].GetUint();
		includeID      = jsonConfig["settings"]["include_ids"].GetBool();
		if (! jsonConfig["settings"]["compress"].IsString()) {
			cerr << "\"compress\" should be any of \"gzip\",\"deflate\",\"none\" in JSON file." << endl;
			return -1;
		}
		compressOpt    = jsonConfig["settings"]["compress"].GetString();
		projectName    = jsonConfig["settings"]["name"].GetString();
		projectVersion = jsonConfig["settings"]["version"].GetString();
		projectDesc    = jsonConfig["settings"]["description"].GetString();
		if (jsonConfig["settings"].HasMember("bounding_box")) {
			hasClippingBox = true; clippingBoxFromJSON = true;
			minLon = jsonConfig["settings"]["bounding_box"][0].GetDouble();
			minLat = jsonConfig["settings"]["bounding_box"][1].GetDouble();
			maxLon = jsonConfig["settings"]["bounding_box"][2].GetDouble();
			maxLat = jsonConfig["settings"]["bounding_box"][3].GetDouble();
			clippingBox = Box(geom::make<Point>(minLon, lat2latp(minLat)),
				              geom::make<Point>(maxLon, lat2latp(maxLat)));
		}

		// Check config is valid
		if (endZoom > baseZoom) { cerr << "maxzoom must be the same or smaller than basezoom." << endl; return -1; }
		if (! compressOpt.empty()) {
			if      (compressOpt == "gzip"   ) { gzip = true;  }
			else if (compressOpt == "deflate") { gzip = false; }
			else if (compressOpt == "none"   ) { compress = false; }
			else {
				cerr << "\"compress\" should be any of \"gzip\",\"deflate\",\"none\" in JSON file." << endl;
				return -1;
			}
		}

		// Layers
		rapidjson::Value& layerHash = jsonConfig["layers"];
		for (rapidjson::Value::MemberIterator it = layerHash.MemberBegin(); it != layerHash.MemberEnd(); ++it) {

			// Basic layer settings
			string layerName = it->name.GetString();
			int minZoom = it->value["minzoom"].GetInt();
			int maxZoom = it->value["maxzoom"].GetInt();
			string writeTo = it->value.HasMember("write_to") ? it->value["write_to"].GetString() : "";
			int   simplifyBelow = it->value.HasMember("simplify_below") ? it->value["simplify_below"].GetInt()    : 0;
			double simplifyLevel = it->value.HasMember("simplify_level") ? it->value["simplify_level"].GetDouble() : 0.01;
			double simplifyLength = it->value.HasMember("simplify_length") ? it->value["simplify_length"].GetDouble() : 0.0;
			double simplifyRatio = it->value.HasMember("simplify_ratio") ? it->value["simplify_ratio"].GetDouble() : 1.0;
			uint layerNum = osmObject.addLayer(layerName, minZoom, maxZoom,
					simplifyBelow, simplifyLevel, simplifyLength, simplifyRatio, writeTo);
			cout << "Layer " << layerName << " (z" << minZoom << "-" << maxZoom << ")";
			if (it->value.HasMember("write_to")) { cout << " -> " << it->value["write_to"].GetString(); }
			cout << endl;

			// External layer sources
			if (it->value.HasMember("source")) {
				if (!hasClippingBox) {
					cerr << "Can't read shapefiles unless a bounding box is provided." << endl;
					return EXIT_FAILURE;
				}
				vector<string> sourceColumns;
				if (it->value.HasMember("source_columns")) {
					for (uint i=0; i<it->value["source_columns"].Size(); i++) {
						sourceColumns.push_back(it->value["source_columns"][i].GetString());
					}
				}
				bool indexed=false; if (it->value.HasMember("index")) {
					indexed=it->value["index"].GetBool();
					indices[layerName]=RTree();
				}
				string indexName = it->value.HasMember("index_column") ? it->value["index_column"].GetString() : "";
				readShapefile(it->value["source"].GetString(), sourceColumns, clippingBox, tileIndex,
				              cachedGeometries, cachedGeometryNames, baseZoom, layerNum, layerName, indexed, indices, indexName);
			}
		}
	} catch (...) {
		cerr << "Couldn't find expected details in JSON file." << endl;
		return -1;
	}

	// ---- Call init_function of Lua logic

	luaState("if init_function~=nil then init_function() end");

	// ----	Read significant node tags

	vector<string> nodeKeyVec = luaState["node_keys"];
	unordered_set<string> nodeKeys(nodeKeyVec.begin(), nodeKeyVec.end());

	// ----	Initialise mbtiles if required
	
	MBTiles mbtiles;
	if (sqlite) {
		mbtiles.open(&outputFile);
		mbtiles.writeMetadata("name",projectName);
		mbtiles.writeMetadata("type","baselayer");
		mbtiles.writeMetadata("version",projectVersion);
		mbtiles.writeMetadata("description",projectDesc);
		mbtiles.writeMetadata("format","pbf");
		if (jsonConfig["settings"].HasMember("metadata")) {
			const rapidjson::Value &md = jsonConfig["settings"]["metadata"];
			for(rapidjson::Value::ConstMemberIterator it=md.MemberBegin(); it != md.MemberEnd(); ++it) {
				if (it->value.IsString()) {
					mbtiles.writeMetadata(it->name.GetString(), it->value.GetString());
				} else {
					rapidjson::StringBuffer strbuf;
					rapidjson::Writer<rapidjson::StringBuffer> writer(strbuf);
					it->value.Accept(writer);
					mbtiles.writeMetadata(it->name.GetString(), strbuf.GetString());
				}
			}
		}
	}

	// ----	Read all PBFs
	
	for (auto inputFile : inputFiles) {
	
		// ----	Read PBF
		// note that the order of reading and processing is:
		//  1) output nodes -> (remember current position for rewinding to ways) (skip ways) -> (just remember all ways in any relation),
		//  2) output ways, and also construct nodeId list for each way in relation -> output relations

		cout << "Reading " << inputFile << endl;

		fstream infile(inputFile, ios::in | ios::binary);
		if (!infile) { cerr << "Couldn't open .pbf file " << inputFile << endl; return -1; }
		HeaderBlock block;
		readBlock(&block, &infile);

		PrimitiveBlock pb;
		PrimitiveGroup pg;
		DenseNodes dense;
		Way pbfWay;
		vector<string> strings(0);
		uint ct=0;
		int64_t nodeId;
		bool checkedRelations = false;
		int wayPosition = -1;
		unordered_set<WayID> waysInRelation;

		while (true) {
			int blockStart = infile.tellg();
			readBlock(&pb, &infile);
			if (infile.eof()) {
				if (!checkedRelations) {
					checkedRelations = true;
				} else {
					break;
				}
				infile.clear();
				infile.seekg(wayPosition);
				continue;
			}

			// Read the string table, and pre-calculate the positions of valid node keys
			osmObject.readStringTable(&pb);
			unordered_set<int> nodeKeyPositions;
			for (auto it : nodeKeys) {
				nodeKeyPositions.insert(osmObject.findStringPosition(it));
			}

			for (int i=0; i<pb.primitivegroup_size(); i++) {
				pg = pb.primitivegroup(i);
				cout << "Block " << ct << " group " << i << " ways " << pg.ways_size() << " relations " << pg.relations_size() << "        \r";
				cout.flush();

				// ----	Read nodes

				if (pg.has_dense()) {
					nodeId  = 0;
					int lon = 0;
					int lat = 0;
					int kvPos = 0;
					dense = pg.dense();
					for (int j=0; j<dense.id_size(); j++) {
						nodeId += dense.id(j);
						lon    += dense.lon(j);
						lat    += dense.lat(j);
						LatpLon node = { int(lat2latp(double(lat)/10000000.0)*10000000.0), lon };
						nodes.insert_back(nodeId, node);
						bool significant = false;
						int kvStart = kvPos;
						if (dense.keys_vals_size()>0) {
							while (dense.keys_vals(kvPos)>0) {
								if (nodeKeyPositions.find(dense.keys_vals(kvPos)) != nodeKeyPositions.end()) {
									significant = true;
								}
								kvPos+=2;
							}
							kvPos++;
						}
						// For tagged nodes, call Lua, then save the OutputObject
						if (significant) {
							osmObject.setNode(nodeId, &dense, kvStart, kvPos-1, node);
							luaState["node_function"](&osmObject);
							if (!osmObject.empty()) {
								uint32_t index = latpLon2index(node, baseZoom);
								for (auto jt = osmObject.outputs.begin(); jt != osmObject.outputs.end(); ++jt) {
									tileIndex[index].push_back(*jt);
								}
							}
						}
					}
					continue;
				}

				// ----	Remember the position and skip ways

				if (!checkedRelations && pg.ways_size() > 0) {
					if (wayPosition == -1) {
						wayPosition = blockStart;
					}
					continue;
				}

				// ----	Remember all ways in any relation

				if (!checkedRelations && pg.relations_size() > 0) {
					for (int j=0; j<pg.relations_size(); j++) {
						Relation pbfRelation = pg.relations(j);
						int64_t lastID = 0;
						for (int n = 0; n < pbfRelation.memids_size(); n++) {
							lastID += pbfRelation.memids(n);
							if (pbfRelation.types(n) != Relation_MemberType_WAY) { continue; }
							WayID wayId = static_cast<WayID>(lastID);
							waysInRelation.insert(wayId);
						}
					}
					continue;
				}

				if (!checkedRelations) {
					// Nothing to do
					break;
				}

				// ----	Read ways

				if (pg.ways_size() > 0) {
					for (int j=0; j<pg.ways_size(); j++) {
						pbfWay = pg.ways(j);
						WayID wayId = static_cast<WayID>(pbfWay.id());

						// Assemble nodelist
						nodeId = 0;
						NodeVec nodeVec;
						for (int k=0; k<pbfWay.refs_size(); k++) {
							nodeId += pbfWay.refs(k);
							nodeVec.push_back(static_cast<NodeID>(nodeId));
						}

						osmObject.setWay(&pbfWay, &nodeVec);
						luaState["way_function"](&osmObject);

						if (!osmObject.empty() || waysInRelation.count(wayId)) {
							// Store the way's nodes in the global way store
							ways.insert_back(wayId, nodeVec);
						}

						if (!osmObject.empty()) {
							// create a list of tiles this way passes through (tileSet)
							unordered_set<uint32_t> tileSet;
							insertIntermediateTiles(osmStore.nodeListLinestring(nodeVec), baseZoom, tileSet);

							// then, for each tile, store the OutputObject for each layer
							bool polygonExists = false;
							for (auto it = tileSet.begin(); it != tileSet.end(); ++it) {
								uint32_t index = *it;
								for (auto jt = osmObject.outputs.begin(); jt != osmObject.outputs.end(); ++jt) {
									if (jt->geomType == POLYGON) {
										polygonExists = true;
										continue;
									}
									tileIndex[index].push_back(*jt);
								}
							}

							// for polygon, fill inner tiles
							if (polygonExists) {
								fillCoveredTiles(tileSet);
								for (auto it = tileSet.begin(); it != tileSet.end(); ++it) {
									uint32_t index = *it;
									for (auto jt = osmObject.outputs.begin(); jt != osmObject.outputs.end(); ++jt) {
										if (jt->geomType != POLYGON) continue;
										tileIndex[index].push_back(*jt);
									}
								}
							}
						}
					}
					continue;
				}

				// ----	Read relations
				//		(just multipolygons for now; we should do routes in time)

				if (pg.relations_size() > 0) {
					int typeKey = osmObject.findStringPosition("type");
					int mpKey   = osmObject.findStringPosition("multipolygon");
					int innerKey= osmObject.findStringPosition("inner");
					//int outerKey= osmObject.findStringPosition("outer");
					if (typeKey >-1 && mpKey>-1) {
						for (int j=0; j<pg.relations_size(); j++) {
							Relation pbfRelation = pg.relations(j);
							if (find(pbfRelation.keys().begin(), pbfRelation.keys().end(), typeKey) == pbfRelation.keys().end()) { continue; }
							if (find(pbfRelation.vals().begin(), pbfRelation.vals().end(), mpKey  ) == pbfRelation.vals().end()) { continue; }

							// Read relation members
							WayVec outerWayVec, innerWayVec;
							int64_t lastID = 0;
							for (int n=0; n < pbfRelation.memids_size(); n++) {
								lastID += pbfRelation.memids(n);
								if (pbfRelation.types(n) != Relation_MemberType_WAY) { continue; }
								int32_t role = pbfRelation.roles_sid(n);
								// if (role != innerKey && role != outerKey) { continue; }
								// ^^^^ commented out so that we don't die horribly when a relation has no outer way
								WayID wayId = static_cast<WayID>(lastID);
								(role == innerKey ? innerWayVec : outerWayVec).push_back(wayId);
							}

							osmObject.setRelation(&pbfRelation, &outerWayVec, &innerWayVec);
							luaState["way_function"](&osmObject);
							if (!osmObject.empty()) {
								WayID relID = osmObject.osmID;
								// Store the relation members in the global relation store
								relations.insert_front(relID, outerWayVec, innerWayVec);

								// for each tile the relation may cover, put the output objects.
								unordered_set<uint32_t> tileSet;
								MultiPolygon mp = osmStore.wayListMultiPolygon(outerWayVec, innerWayVec);
								if (mp.size() == 1) {
									insertIntermediateTiles(mp[0].outer(), baseZoom, tileSet);
									fillCoveredTiles(tileSet);
								} else {
									for (Polygon poly: mp) {
										unordered_set<uint32_t> tileSetTmp;
										insertIntermediateTiles(poly.outer(), baseZoom, tileSetTmp);
										fillCoveredTiles(tileSetTmp);
										tileSet.insert(tileSetTmp.begin(), tileSetTmp.end());
									}
								}

								for (auto it = tileSet.begin(); it != tileSet.end(); ++it) {
									uint32_t index = *it;
									for (auto jt = osmObject.outputs.begin(); jt != osmObject.outputs.end(); ++jt) {
										tileIndex[index].push_back(*jt);
									}
								}
							}
						}
					}
					continue;
				}

				// Everything should be ended
				break;
			}
			ct++;
		}
		cout << endl;
		infile.close();
	}
	osmStore.reportSize();

	// ----	Write out each tile

	// Loop through zoom levels
	for (uint zoom=startZoom; zoom<=endZoom; zoom++) {
		// Create list of tiles, and the data in them
		map< uint, vector<OutputObject> > *tileIndexPtr;
		map< uint, vector<OutputObject> > generatedIndex;
		if (zoom==baseZoom) {
			// ----	Sort each tile
			for (auto it = tileIndex.begin(); it != tileIndex.end(); ++it) {
				auto &ooset = it->second;
				sort(ooset.begin(), ooset.end());
				ooset.erase(unique(ooset.begin(), ooset.end()), ooset.end());
			}
			// at z14, we can just use tileIndex
			tileIndexPtr = &tileIndex;
		} else {
			// otherwise, we need to run through the z14 list, and assign each way
			// to a tile at our zoom level
			for (auto it = tileIndex.begin(); it!= tileIndex.end(); ++it) {
				uint index = it->first;
				uint tilex = (index >> 16  ) / pow(2, baseZoom-zoom);
				uint tiley = (index & 65535) / pow(2, baseZoom-zoom);
				uint newIndex = (tilex << 16) + tiley;
				const vector<OutputObject> &ooset = it->second;
				for (auto jt = ooset.begin(); jt != ooset.end(); ++jt) {
					generatedIndex[newIndex].push_back(*jt);
				}
			}
			// sort each new tile
			for (auto it = generatedIndex.begin(); it != generatedIndex.end(); ++it) {
				auto &ooset = it->second;
				sort(ooset.begin(), ooset.end());
				ooset.erase(unique(ooset.begin(), ooset.end()), ooset.end());
			}
			tileIndexPtr = &generatedIndex;
		}

		// Output procedure
		auto outputProc = [&](uint threadId) {
			// Loop through tiles
			uint tc = 0;
			for (auto it = tileIndexPtr->begin(); it != tileIndexPtr->end(); ++it) {
				if (threadId == 0 && (tc % 100) == 0) {
					cout << "Zoom level " << zoom << ", writing tile " << tc << " of " << tileIndexPtr->size() << "               \r";
					cout.flush();
				}
				if (tc++ % threadNum != threadId) continue;

				// Create tile
				vector_tile::Tile tile;
				uint index = it->first;
				TileBbox bbox(index,zoom);
				const vector<OutputObject> &ooList = it->second;
				if (clippingBoxFromJSON && (maxLon<=bbox.minLon || minLon>=bbox.maxLon || maxLat<=bbox.minLat || minLat>=bbox.maxLat)) { continue; }

				// Loop through layers
				for (auto lt = osmObject.layerOrder.begin(); lt != osmObject.layerOrder.end(); ++lt) {
					vector<string> keyList;
					vector<vector_tile::Tile_Value> valueList;
					vector_tile::Tile_Layer *vtLayer = tile.add_layers();

					for (auto mt = lt->begin(); mt != lt->end(); ++mt) {
						uint layerNum = *mt;
						LayerDef ld = osmObject.layers[layerNum];
						if (zoom<ld.minzoom || zoom>ld.maxzoom) { continue; }
						double simplifyLevel = 0;
						if (zoom < ld.simplifyBelow) {
							if (ld.simplifyLength > 0) {
								uint tileY = index & 65535;
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
						auto ooListSameLayer = equal_range(ooList.begin(), ooList.end(), OutputObject(POINT, layerNum, 0), layerComp);
						// Loop through output objects
						for (auto jt = ooListSameLayer.first; jt != ooListSameLayer.second; ++jt) {
							if (jt->geomType == POINT) {
								vector_tile::Tile_Feature *featurePtr = vtLayer->add_features();
								jt->buildNodeGeometry(nodes.at(jt->objectID), &bbox, featurePtr);
								jt->writeAttributes(&keyList, &valueList, featurePtr);
								if (includeID) { featurePtr->set_id(jt->objectID); }
							} else {
								try {
									Geometry g = jt->buildWayGeometry(osmStore, &bbox, cachedGeometries);

									// If a object is a polygon or a linestring that is followed by
									// other objects with the same geometry type and the same attributes,
									// the following objects are merged into the first object, by taking union of geometries.
									auto gTyp = jt->geomType;
									if (gTyp == POLYGON || gTyp == CACHED_POLYGON) {
										MultiPolygon &gAcc = boost::get<MultiPolygon>(g);
										while (jt+1 != ooListSameLayer.second &&
												(jt+1)->geomType == gTyp &&
												(jt+1)->attributes == jt->attributes) {
											jt++;
											MultiPolygon gNew = boost::get<MultiPolygon>(jt->buildWayGeometry(osmStore, &bbox, cachedGeometries));
											MultiPolygon gTmp;
											geom::union_(gAcc, gNew, gTmp);
											gAcc = move(gTmp);
										}
									}
									if (gTyp == LINESTRING || gTyp == CACHED_LINESTRING) {
										MultiLinestring &gAcc = boost::get<MultiLinestring>(g);
										while (jt+1 != ooListSameLayer.second &&
												(jt+1)->geomType == gTyp &&
												(jt+1)->attributes == jt->attributes) {
											jt++;
											MultiLinestring gNew = boost::get<MultiLinestring>(jt->buildWayGeometry(osmStore, &bbox, cachedGeometries));
											MultiLinestring gTmp;
											geom::union_(gAcc, gNew, gTmp);
											gAcc = move(gTmp);
										}
									}

									vector_tile::Tile_Feature *featurePtr = vtLayer->add_features();
									WriteGeometryVisitor w(&bbox, featurePtr, simplifyLevel);
									boost::apply_visitor(w, g);
									if (featurePtr->geometry_size()==0) { vtLayer->mutable_features()->RemoveLast(); continue; }
									jt->writeAttributes(&keyList, &valueList, featurePtr);
									if (includeID) { featurePtr->set_id(jt->objectID); }
								} catch (...) {
									if (verbose)  {
										cerr << "Exception when writing output object " << jt->objectID << " of type " << jt->geomType << endl;
										if (relations.count(jt->objectID)) {
											const auto &wayList = relations.at(jt->objectID);
											for (auto et = wayList.outerBegin; et != wayList.outerEnd; ++et) {
												if (ways.count(*et)==0) { cerr << " - couldn't find constituent way " << *et << endl; }
											}
											for (auto et = wayList.innerBegin; et != wayList.innerEnd; ++et) {
												if (ways.count(*et)==0) { cerr << " - couldn't find constituent way " << *et << endl; }
											}
										}
									}
								}
							}
						}
					}

					// If there are any objects, then add tags
					if (vtLayer->features_size()>0) {
						vtLayer->set_name(osmObject.layers[lt->at(0)].name);
						vtLayer->set_version(1);
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

				// Write to file or sqlite

				string data, compressed;
				if (sqlite) {
					// Write to sqlite
					tile.SerializeToString(&data);
					if (compress) { compressed = compress_string(data, Z_DEFAULT_COMPRESSION, gzip); }
					mbtiles.saveTile(zoom, bbox.tilex, bbox.tiley, compress ? &compressed : &data);

				} else {
					// Write to file
					stringstream dirname, filename;
					dirname  << outputFile << "/" << zoom << "/" << bbox.tilex;
					filename << outputFile << "/" << zoom << "/" << bbox.tilex << "/" << bbox.tiley << ".pbf";
					boost::filesystem::create_directories(dirname.str());
					fstream outfile(filename.str(), ios::out | ios::trunc | ios::binary);
					if (compress) {
						tile.SerializeToString(&data);
						outfile << compress_string(data, Z_DEFAULT_COMPRESSION, gzip);
					} else {
						if (!tile.SerializeToOstream(&outfile)) { cerr << "Couldn't write to " << filename.str() << endl; return -1; }
					}
					outfile.close();
				}
			}
			return 0;
		};

		// Multi thread processing loop
		vector<thread> worker;
		for (uint threadId = 0; threadId < threadNum; threadId++)
			worker.emplace_back(outputProc, threadId);
		for (auto &t: worker) t.join();
	}

	cout << endl << "Filled the tileset with good things at " << outputFile << endl;
	google::protobuf::ShutdownProtobufLibrary();

	// ---- Call exit_function of Lua logic
	luaState("if exit_function~=nil then exit_function() end");
}
