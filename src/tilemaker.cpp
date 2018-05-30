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

#ifdef _MSC_VER
typedef unsigned uint;
#endif

// Lua
extern "C" {
	#include "lua.h"
    #include "lualib.h"
    #include "lauxlib.h"
}
#include "kaguya.hpp"

#include "geomtypes.h"

// Tilemaker code
#include "helpers.h"
#include "coordinates.h"

#include "osm_store.h"
#include "output_object.h"
#include "osm_object.h"
#include "mbtiles.h"
#include "write_geometry.h"

#include "shared_data.h"
#include "read_pbf.h"
#include "read_shp.h"
#include "tile_worker.h"

// Namespaces
using namespace std;
namespace po = boost::program_options;
namespace geom = boost::geometry;

kaguya::State luaState;

int lua_error_handler(int errCode, const char *errMessage)
{
	cerr << "lua runtime error: " << errMessage << endl;
	std::string traceback = luaState["debug"]["traceback"];
	cerr << "traceback: " << traceback << endl;
	exit(0);
}

void loadExternalShpFiles(class Config &config, bool hasClippingBox, const Box &clippingBox,
                map< uint64_t, vector<OutputObject> > &tileIndex, 
				std::vector<Geometry> &cachedGeometries,
				OSMObject &osmObject)
{
	for(size_t layerNum=0; layerNum<config.layers.size(); layerNum++)	
	{
		// External layer sources
		LayerDef &layer = config.layers[layerNum];
		if(layer.indexed)
			osmObject.indices->operator[](layer.name)=RTree();

		if (layer.source.size()>0) {
			if (!hasClippingBox) {
				cerr << "Can't read shapefiles unless a bounding box is provided." << endl;
				exit(EXIT_FAILURE);
			}
			readShapefile(layer.source, layer.sourceColumns, clippingBox, tileIndex,
			              cachedGeometries,
			              osmObject,
			              config.baseZoom, layerNum, layer.name, layer.indexed,
			              layer.indexName);
		}
	}
}

int main(int argc, char* argv[]) {

	// ----	Initialise data collections

	OSMStore osmStore;									// global OSM store

	map<string, RTree> indices;						// boost::geometry::index objects for shapefile indices
	std::vector<Geometry> cachedGeometries;					// prepared boost::geometry objects (from shapefiles)
	map<uint, string> cachedGeometryNames;			//  | optional names for each one

	map< uint64_t, vector<OutputObject> > tileIndex;				// objects to be output

	// ----	Read command-line options
	
	vector<string> inputFiles;
	string luaFile;
	string jsonFile;
	uint threadNum;
	string outputFile;
	bool verbose = false, sqlite= false;

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
    try {
        po::store(po::command_line_parser(argc, argv).options(desc).positional(p).run(), vm);
    }
    catch (const po::unknown_option& ex) {
        cerr << "Unknown option: " << ex.get_option_name() << endl;
        return -1;
    }
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
	#ifdef COMPACT_WAYS
	cout << "tilemaker compiled without 64-bit way support, use 'osmium renumber' first if working with OpenStreetMap-sourced data" << endl;
	#endif

	// ---- Check config
	
	if (!boost::filesystem::exists(jsonFile)) { cerr << "Couldn't open .json config: " << jsonFile << endl; return -1; }
	if (!boost::filesystem::exists(luaFile )) { cerr << "Couldn't open .lua script: "  << luaFile  << endl; return -1; }

	// ----	Read bounding box from first .pbf

	bool hasClippingBox = false;
	double minLon=0.0, maxLon=0.0, minLat=0.0, maxLat=0.0;
	int ret = ReadPbfBoundingBox(inputFiles[0], minLon, maxLon, 
		minLat, maxLat, hasClippingBox);
	if(ret != 0) return ret;
	Box clippingBox;
	if(hasClippingBox)
	{
		clippingBox = Box(geom::make<Point>(minLon, lat2latp(minLat)),
		                  geom::make<Point>(maxLon, lat2latp(maxLat)));
	}
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

	// ----	Read JSON config

	rapidjson::Document jsonConfig;
	class Config config;
	try {
		FILE* fp = fopen(jsonFile.c_str(), "r");
		char readBuffer[65536];
		rapidjson::FileReadStream is(fp, readBuffer, sizeof(readBuffer));
		jsonConfig.ParseStream(is);
		if (jsonConfig.HasParseError()) { cerr << "Invalid JSON file." << endl; return -1; }
		fclose(fp);

		config.readConfig(jsonConfig, hasClippingBox, clippingBox);

	} catch (...) {
		cerr << "Couldn't find expected details in JSON file." << endl;
		return -1;
	}

	if(hasClippingBox)
	{
		config.minLon = minLon;
		config.maxLon = maxLon;
		config.minLat = minLat;
		config.maxLat = maxLat;
	}

	// ----	Initialise SharedData

	class SharedData sharedData(config, &osmStore);
	sharedData.threadNum = threadNum;
	sharedData.outputFile = outputFile;
	sharedData.verbose = verbose;
	sharedData.sqlite = sqlite;
	sharedData.cachedGeometries = &cachedGeometries;

	OSMObject osmObject(config, luaState, &indices, &cachedGeometries, &cachedGeometryNames, &osmStore);

	// ---- Load external shp files

	loadExternalShpFiles(config, hasClippingBox, clippingBox, tileIndex, cachedGeometries, osmObject);

	// ---- Call init_function of Lua logic

	luaState("if init_function~=nil then init_function() end");

	// ----	Read significant node tags

	vector<string> nodeKeyVec = luaState["node_keys"];
	unordered_set<string> nodeKeys(nodeKeyVec.begin(), nodeKeyVec.end());

	// ----	Initialise mbtiles if required
	
	if (sharedData.sqlite) {
		ostringstream bounds;
		bounds << fixed << sharedData.config.minLon << "," << sharedData.config.minLat << "," << sharedData.config.maxLon << "," << sharedData.config.maxLat;
		sharedData.mbtiles.open(&sharedData.outputFile);
		sharedData.mbtiles.writeMetadata("name",sharedData.config.projectName);
		sharedData.mbtiles.writeMetadata("type","baselayer");
		sharedData.mbtiles.writeMetadata("version",sharedData.config.projectVersion);
		sharedData.mbtiles.writeMetadata("description",sharedData.config.projectDesc);
		sharedData.mbtiles.writeMetadata("format","pbf");
		sharedData.mbtiles.writeMetadata("bounds",bounds.str());
		sharedData.mbtiles.writeMetadata("minzoom",to_string(sharedData.config.startZoom));
		sharedData.mbtiles.writeMetadata("maxzoom",to_string(sharedData.config.endZoom));
		if (!sharedData.config.defaultView.empty()) { sharedData.mbtiles.writeMetadata("center",sharedData.config.defaultView); }
	}

	// ----	Read all PBFs
	
	for (auto inputFile : inputFiles) {
	
		cout << "Reading " << inputFile << endl;

		int ret = ReadPbfFile(inputFile, nodeKeys, tileIndex, osmObject);
		if(ret != 0)
			return ret;
	}
	osmStore.reportSize();

	// ----	Write out each tile

	// Loop through zoom levels
	for (uint zoom=sharedData.config.startZoom; zoom<=sharedData.config.endZoom; zoom++) {
		// Create list of tiles, and the data in them
		map< uint64_t, vector<OutputObject> > generatedIndex;
		if (zoom==sharedData.config.baseZoom) {
			// ----	Sort each tile
			for (auto it = tileIndex.begin(); it != tileIndex.end(); ++it) {
				auto &ooset = it->second;
				sort(ooset.begin(), ooset.end());
				ooset.erase(unique(ooset.begin(), ooset.end()), ooset.end());
			}
			// at z14, we can just use tileIndex
			sharedData.tileIndexForZoom = &tileIndex;
		} else {
			// otherwise, we need to run through the z14 list, and assign each way
			// to a tile at our zoom level
			for (auto it = tileIndex.begin(); it!= tileIndex.end(); ++it) {
				uint64_t index = it->first;
				uint64_t tilex = (index >> 32  ) / pow(2, sharedData.config.baseZoom-zoom);
				uint64_t tiley = (index & 4294967295) / pow(2, sharedData.config.baseZoom-zoom);
				uint64_t newIndex = (tilex << 32) + tiley;
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
			sharedData.tileIndexForZoom = &generatedIndex;

		}

		sharedData.zoom = zoom;
		if(threadNum == 1) {
			// Single thread (is easier to debug)
			outputProc(0, &sharedData);
		}
		else {

			// Multi thread processing loop
			vector<thread> worker;
			for (uint threadId = 0; threadId < threadNum; threadId++)
				worker.emplace_back(outputProc, threadId, &sharedData);
			for (auto &t: worker) t.join();

		}
		sharedData.tileIndexForZoom = nullptr;
	}

	// ----	Close tileset

	if (sqlite) {
		// Write mbtiles 1.3+ json object
		sharedData.mbtiles.writeMetadata("json", osmObject.serialiseLayerJSON());

		// Write user-defined metadata
		if (jsonConfig["settings"].HasMember("metadata")) {
			const rapidjson::Value &md = jsonConfig["settings"]["metadata"];
			for(rapidjson::Value::ConstMemberIterator it=md.MemberBegin(); it != md.MemberEnd(); ++it) {
				if (it->value.IsString()) {
					sharedData.mbtiles.writeMetadata(it->name.GetString(), it->value.GetString());
				} else {
					rapidjson::StringBuffer strbuf;
					rapidjson::Writer<rapidjson::StringBuffer> writer(strbuf);
					it->value.Accept(writer);
					sharedData.mbtiles.writeMetadata(it->name.GetString(), strbuf.GetString());
				}
			}
		}
		sharedData.mbtiles.close();
	}
	google::protobuf::ShutdownProtobufLibrary();

	// Call exit_function of Lua logic
	luaState("if exit_function~=nil then exit_function() end");

	cout << endl << "Filled the tileset with good things at " << sharedData.outputFile << endl;
}

