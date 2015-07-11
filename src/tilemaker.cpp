/*	
	tilemaker
	Richard Fairhurst, June 2015
*/

// C++ includes
#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <unordered_set>
#include <string>
#include <cmath>

// Other utilities
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/filereadstream.h"
#include "sqlite_modern_cpp.h"

// Protobuf
#include "osmformat.pb.h"
#include "vector_tile.pb.h"

// Lua
extern "C" {
	#include "lua.h"
    #include "lualib.h"
    #include "lauxlib.h"
}
#include <luabind/luabind.hpp>
#include <luabind/function.hpp> 

// boost::geometry
#include <boost/geometry.hpp>
#include <boost/geometry/algorithms/intersection.hpp>
#include <boost/geometry/geometries/geometries.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
typedef boost::geometry::model::d2::point_xy<double> Point; 
typedef boost::geometry::model::linestring<Point> Linestring;
typedef boost::geometry::model::polygon<Point> Polygon;
typedef boost::geometry::model::multi_polygon<Polygon> MultiPolygon;
typedef boost::geometry::model::multi_linestring<Linestring> MultiLinestring;
typedef boost::geometry::model::box<Point> Box;
typedef boost::geometry::ring_type<Polygon>::type Ring;
typedef boost::geometry::interior_type<Polygon>::type InteriorRing;

// Namespaces
using namespace std;
using namespace sqlite;
namespace po = boost::program_options;
namespace geom = boost::geometry;

// Tilemaker code
#include "helpers.cpp"
#include "pbf_blocks.cpp"
#include "coordinates.cpp"

typedef std::map< uint32_t, LatLon > node_container_t;

#include "output_object.cpp"
#include "osm_object.cpp"
#include "mbtiles.cpp"

int main(int argc, char* argv[]) {

	// ----	Initialise data collections

	node_container_t nodes;						// lat/lon for all nodes (node ids fit into uint32, at least for now)
	map< uint32_t, WayStore > ways;						// node list for all ways
	map< uint, unordered_set<OutputObject> > tileIndex;	// objects to be output
	map< uint32_t, unordered_set<OutputObject> > relationOutputObjects;	// outputObjects for multipolygons (saved for processing later as ways)
	map< uint32_t, vector<uint32_t> > wayRelations;		// for each way, which relations it's in (therefore we need to keep them)

	// ----	Read command-line options
	
	bool sqlite=false;
	vector<string> inputFiles;
	string outputFile;
	string luaFile;
	string jsonFile;

	po::options_description desc("tilemaker (c) 2015 Richard Fairhurst\nConvert OpenStreetMap .pbf files into vector tiles\n\nAvailable options");
	desc.add_options()
		("help",                                                                 "show help message")
		("input",  po::value< vector<string> >(&inputFiles),                     "source .osm.pbf file")
		("output", po::value< string >(&outputFile),                             "target directory or .mbtiles/.sqlite file")
		("config", po::value< string >(&jsonFile)->default_value("config.json"), "config JSON file")
		("process",po::value< string >(&luaFile)->default_value("process.lua"),  "tag-processing Lua file");
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
	
	// ----	Initialise Lua

    lua_State *luaState = luaL_newstate();
    luaL_openlibs(luaState);
    luaL_dofile(luaState, luaFile.c_str());
    luabind::open(luaState);
	luabind::module(luaState) [
	luabind::class_<OSMObject>("OSM")
		.def("Find", &OSMObject::Find)
		.def("Holds", &OSMObject::Holds)
		.def("Id", &OSMObject::Id)
		.def("Layer", &OSMObject::Layer)
		.def("Attribute", &OSMObject::Attribute)
		.def("AttributeNumeric", &OSMObject::AttributeNumeric)
		.def("AttributeBoolean", &OSMObject::AttributeBoolean)
	];
	OSMObject osmObject(luaState);

	// ----	Read JSON config

	uint baseZoom, startZoom, endZoom;
	string projectName, projectVersion, projectDesc;
	bool includeID = false, compress = true;
	rapidjson::Document jsonConfig;
	try {
		FILE* fp = fopen(jsonFile.c_str(), "r");
		char readBuffer[65536];
		rapidjson::FileReadStream is(fp, readBuffer, sizeof(readBuffer));
		jsonConfig.ParseStream(is);
		if (jsonConfig.HasParseError()) { cerr << "Invalid JSON file." << endl; return -1; }
		fclose(fp);
	
		rapidjson::Value& layerHash = jsonConfig["layers"];
		for (rapidjson::Value::MemberIterator it = layerHash.MemberBegin(); it != layerHash.MemberEnd(); ++it) {
		    // work with (*itr)["status"], etc.
			string layerName = it->name.GetString();
			int minZoom = it->value["minzoom"].GetInt();
			int maxZoom = it->value["maxzoom"].GetInt();
			osmObject.addLayer(layerName, minZoom, maxZoom);
			cout << "Layer " << layerName << " (z" << minZoom << "-" << maxZoom << ")" << endl;
		}

		baseZoom       = jsonConfig["settings"]["basezoom"].GetUint();
		startZoom      = jsonConfig["settings"]["minzoom" ].GetUint();
		endZoom        = jsonConfig["settings"]["maxzoom" ].GetUint();
		includeID      = jsonConfig["settings"]["include_ids"].GetBool();
		compress       = jsonConfig["settings"]["compress"].GetBool();
		projectName    = jsonConfig["settings"]["name"].GetString();
		projectVersion = jsonConfig["settings"]["version"].GetString();
		projectDesc    = jsonConfig["settings"]["description"].GetString();
		if (endZoom > baseZoom) { cerr << "maxzoom must be the same or smaller than basezoom." << endl; return -1; }
	} catch (...) {
		cerr << "Couldn't find expected details in JSON file." << endl;
		return -1;
	}

	// ----	Read significant node tags

	unordered_set<string> nodeKeys;
	lua_getglobal( luaState, "node_keys");
	lua_pushnil( luaState );
	while(lua_next( luaState, -2) != 0) {
		string key = lua_tostring( luaState, -1 );
		lua_pop( luaState, 1);
		nodeKeys.insert(key);
	}

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
	
		cout << "Reading " << inputFile << endl;
		fstream infile(inputFile, ios::in | ios::binary);
		if (!infile) { cerr << "Couldn't open .pbf input file." << endl; return -1; }
		HeaderBlock block;
		readBlock(&block, &infile);

		// ---- Read PBF
		//		note that the order of reading is nodes, (skip ways), relations, (rewind), ways

		PrimitiveBlock pb;
		PrimitiveGroup pg;
		DenseNodes dense;
		Way pbfWay;
		vector<string> strings(0);
		uint i,j,k,ct=0;
		int64_t nodeId;
		bool processedRelations = false;
		int wayPosition = -1;

		while (!infile.eof()) {
			int blockStart = infile.tellg();
			readBlock(&pb, &infile);
			if (infile.eof() && processedRelations) { break; }
			else if (infile.eof()) {
				processedRelations = true;
				infile.clear();
				infile.seekg(wayPosition);
				readBlock(&pb, &infile);
			}

			// Read the string table, and pre-calculate the positions of valid node keys
			osmObject.readStringTable(&pb);
			unordered_set<int> nodeKeyPositions;
			for (auto it : nodeKeys) {
				nodeKeyPositions.insert(osmObject.findStringPosition(it));
			}
		
			for (i=0; i<pb.primitivegroup_size(); i++) {
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
					for (j=0; j<dense.id_size(); j++) {
						nodeId += dense.id(j);
						lon    += dense.lon(j);
						lat    += dense.lat(j);
						LatLon node = { lat, lon };
						nodes[nodeId] = node;
						bool significant = false;
						int kvStart = kvPos;
						while (dense.keys_vals(kvPos)>0) {
							if (nodeKeyPositions.find(dense.keys_vals(kvPos)) != nodeKeyPositions.end()) {
								significant = true;
							}
							kvPos+=2;
						}
						kvPos++;
						// For tagged nodes, call Lua, then save the OutputObject
						if (significant) {
							osmObject.setNode(nodeId, &dense, kvStart, kvPos-1);
							try { luabind::call_function<int>(luaState, "node_function", &osmObject);
							} catch (const luabind::error &er) {
		    					cerr << er.what() << endl << "-- " << lua_tostring(er.state(), -1) << endl;
								return -1;
							}
							if (!osmObject.empty()) {
								uint32_t index = latLon2index(node, baseZoom);
								for (auto jt = osmObject.outputs.begin(); jt != osmObject.outputs.end(); ++jt) {
									tileIndex[index].insert(*jt);
								}
							}
						}
					}
				}
			
				// ----	Read relations
				// 		(just multipolygons for now; we should do routes in time)

				if (pg.relations_size() > 0 && !processedRelations) {
					int typeKey = osmObject.findStringPosition("type");
					int mpKey   = osmObject.findStringPosition("multipolygon");
					int innerKey= osmObject.findStringPosition("inner");
					int outerKey= osmObject.findStringPosition("outer");
					if (typeKey >-1 && mpKey>-1) {
						for (j=0; j<pg.relations_size(); j++) {
							Relation pbfRelation = pg.relations(j);
							if (find(pbfRelation.keys().begin(), pbfRelation.keys().end(), typeKey) == pbfRelation.keys().end()) { continue; }
							if (find(pbfRelation.vals().begin(), pbfRelation.vals().end(), mpKey  ) == pbfRelation.vals().end()) { continue; }
							osmObject.setRelation(&pbfRelation);
							// Check with Lua if we want it
							try { luabind::call_function<int>(luaState, "way_function", &osmObject);
							} catch (const luabind::error &er) {
		    					cerr << er.what() << endl << "-- " << lua_tostring(er.state(), -1) << endl;
								return -1;
							}
							if (!osmObject.empty()) {
								// put all the ways into relationWays
								osmObject.storeRelationWays(&pbfRelation, &wayRelations, innerKey, outerKey);
								for (auto jt = osmObject.outputs.begin(); jt != osmObject.outputs.end(); ++jt) {
									relationOutputObjects[osmObject.osmID].insert(*jt);
								}
							}
						}
					}
				}
			
				// ----	Read ways

				if (pg.ways_size() > 0 && processedRelations) {
					for (j=0; j<pg.ways_size(); j++) {
						pbfWay = pg.ways(j);
						osmObject.setWay(&pbfWay);
						uint32_t wayId = static_cast<uint32_t>(pbfWay.id());

						// Call Lua to find what layers and tags we want
						try { luabind::call_function<int>(luaState, "way_function", &osmObject);
						} catch (const luabind::error &er) {
	    					cerr << er.what() << endl << "-- " << lua_tostring(er.state(), -1) << endl;
							return -1;
						}

						bool inRelation = wayRelations.count(pbfWay.id()) > 0;
						if (!osmObject.empty() || inRelation) {
							// create a list of tiles this way passes through (tilelist)
							// and save the nodelist in the global way hash
							nodeId = 0;
							vector <uint32_t> nodelist;
							unordered_set <uint32_t> tilelist = {};
							uint lastX, lastY, lastId;
							for (k=0; k<pbfWay.refs_size(); k++) {
								nodeId += pbfWay.refs(k);
								nodelist.push_back(uint32_t(nodeId));
								int tileX = lon2tilex(nodes[nodeId].lon / 10000000.0, baseZoom);
								int tileY = lat2tiley(nodes[nodeId].lat / 10000000.0, baseZoom);
								if (k>0) {
									// Check we're not skipping any tiles, and insert intermediate nodes if so
									// (we should have a simple fill algorithm for polygons, too)
									int dx = abs((int)(tileX-lastX));
									int dy = abs((int)(tileY-lastY));
									if (dx>1 || dy>1 || (dx==1 && dy==1)) {
										insertIntermediateTiles(&tilelist, max(dx,dy), nodes[lastId], nodes[nodeId], baseZoom);
									}
								}
								uint32_t index = tileX * 65536 + tileY;
								tilelist.insert( index );
								lastX = tileX;
								lastY = tileY;
								lastId= nodeId;
							}
							ways[wayId].nodelist = nodelist;
						
							// then, for each tile, store the OutputObject for each layer
							for (auto it = tilelist.begin(); it != tilelist.end(); ++it) {
								uint32_t index = *it;
								for (auto jt = osmObject.outputs.begin(); jt != osmObject.outputs.end(); ++jt) {
									tileIndex[index].insert(*jt);
								}
							}

							// if it's in any relations, do the same for each relation
							if (inRelation) {
								for (auto wt = wayRelations[wayId].begin(); wt != wayRelations[wayId].end(); ++wt) {
									uint32_t relID = *wt;
									// relID is now the relation ID
									for (auto it = tilelist.begin(); it != tilelist.end(); ++it) {
										// index is now the tile index number
										uint32_t index = *it;
										// add all the OutputObjects for this relation into this tile
										for (auto jt = relationOutputObjects[relID].begin(); jt != relationOutputObjects[relID].end(); ++jt) {
											tileIndex[index].insert(*jt);
										}
									}
								}
							}
						}
					}
				} else if (pg.ways_size()>0 && !processedRelations && wayPosition == -1) {
					wayPosition = blockStart;
				}
			}
			ct++;
		}
		cout << endl;
		infile.close();
	}

	// ----	Write out each tile

	// Loop through zoom levels
	for (uint zoom=startZoom; zoom<=endZoom; zoom++) {
		// Create list of tiles, and the data in them
		map< uint, unordered_set<OutputObject> > *tileIndexPtr;
		map< uint, unordered_set<OutputObject> > generatedIndex;
		if (zoom==baseZoom) {
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
				unordered_set<OutputObject> ooset = it->second;
				for (auto jt = ooset.begin(); jt != ooset.end(); ++jt) {
					generatedIndex[newIndex].insert(*jt);
				}
			}
			tileIndexPtr = &generatedIndex;
		}

		// Loop through tiles
		uint tc = 0;
		for (auto it = tileIndexPtr->begin(); it != tileIndexPtr->end(); ++it) {
			if ((tc % 100) == 0) { 
				cout << "Zoom level " << zoom << ", writing tile " << tc << " of " << tileIndexPtr->size() << "               \r";
				cout.flush();
			}
			tc++;

			// Create tile
			vector_tile::Tile tile;
			uint index = it->first;
			TileBbox bbox(index,zoom);
			unordered_set<OutputObject> ooList = it->second;

			// Loop through layers
			for (uint layerNum = 0; layerNum < osmObject.layers.size(); layerNum++) {
				LayerDef ld = osmObject.layers[layerNum];
				if (zoom<ld.minzoom || zoom>ld.maxzoom) { continue; }

				vector<string> keyList;
				vector<vector_tile::Tile_Value> valueList;

				// Loop through output objects
				vector_tile::Tile_Layer *vtLayer = tile.add_layers();
				for (auto jt = ooList.begin(); jt != ooList.end(); ++jt) {
					if (jt->layer != layerNum) { continue; }
					if (jt->geomType == vector_tile::Tile_GeomType_POINT) {
						vector_tile::Tile_Feature *featurePtr = vtLayer->add_features();
						jt->buildNodeGeometry(nodes[jt->osmID], &bbox, featurePtr);
						jt->writeAttributes(&keyList, &valueList, featurePtr);
						if (includeID) { featurePtr->set_id(jt->osmID); }
					} else {
						try {
							vector_tile::Tile_Feature *featurePtr = vtLayer->add_features();
							jt->buildWayGeometry(nodes, &ways, &bbox, featurePtr);
							if (featurePtr->geometry_size()==0) { vtLayer->mutable_features()->RemoveLast(); continue; }
							jt->writeAttributes(&keyList, &valueList, featurePtr);
							if (includeID) { featurePtr->set_id(jt->osmID); }
						} catch (...) {
							cerr << "Exception when writing output object " << jt->osmID << endl;
							for (auto et = jt->outerWays.begin(); et != jt->outerWays.end(); ++et) { 
								if (ways.count(*et)==0) { cerr << " - couldn't find constituent way " << *et << endl; }
							}
							for (auto et = jt->innerWays.begin(); et != jt->innerWays.end(); ++et) { 
								if (ways.count(*et)==0) { cerr << " - couldn't find constituent way " << *et << endl; }
							}
						}
					}
				}

				// If there are any objects, then add tags
				if (vtLayer->features_size()>0) {
					vtLayer->set_name(ld.name);
					vtLayer->set_version(1);
					for (uint j=0; j<keyList.size()  ; j++) {
						vtLayer->add_keys(keyList[j]);
					}
					for (uint j=0; j<valueList.size(); j++) { 
						vector_tile::Tile_Value *v = vtLayer->add_values();
						*v = valueList[j];	// check this actually works!
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
				if (compress) { compressed = compress_string(data); }
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
					outfile << compress_string(data);
				} else {
					if (!tile.SerializeToOstream(&outfile)) { cerr << "Couldn't write to " << filename.str() << endl; return -1; }
				}
				outfile.close();
			}
		}
	}

	cout << endl << "Filled the tileset with good things at " << outputFile << endl;
	google::protobuf::ShutdownProtobufLibrary();
    lua_close(luaState);
}
