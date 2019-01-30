/*! \file */ 

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

#include "geomtypes.h"

// Tilemaker code
#include "helpers.h"
#include "coordinates.h"

#include "output_object.h"
#include "osm_lua_processing.h"
#include "mbtiles.h"
#include "write_geometry.h"

#include "shared_data.h"
#include "read_pbf.h"
#include "read_shp.h"
#include "tile_worker.h"
#include "osm_mem_tiles.h"
#include "osm_disk_tiles.h"
#include "shp_mem_tiles.h"

// Namespaces
using namespace std;
namespace po = boost::program_options;
namespace geom = boost::geometry;

/**
 *\brief The Main function is responsible for command line processing, loading data and starting worker threads.
 *
 * Data is loaded into OsmMemTiles/OsmDiskTiles and ShpMemTiles.
 *
 * Worker threads write the output tiles, and start in the outputProc function.
 */
int main(int argc, char* argv[]) {

	// ----	Read command-line options
	
	vector<string> inputFiles;
	string luaFile;
	string jsonFile;
	uint threadNum;
	string outputFile, argClippingBoxStr;
	bool verbose = false, sqlite= false, combineSimilarObjs = true, tiledInput = false;

	po::options_description desc("tilemaker (c) 2016-2018 Richard Fairhurst and contributors\nConvert OpenStreetMap .pbf files into vector tiles\n\nAvailable options");
	desc.add_options()
		("help",                                                                 "show help message")
		("input",      po::value< vector<string> >(&inputFiles),                     "source .osm.pbf file")
		("output",     po::value< string >(&outputFile),                             "target directory or .mbtiles/.sqlite file")
		("config",     po::value< string >(&jsonFile)->default_value("config.json"), "config JSON file")
		("process",    po::value< string >(&luaFile)->default_value("process.lua"),  "tag-processing Lua file")
		("verbose",    po::bool_switch(&verbose)->default_value(false),              "verbose error output")
		("tiled-input",po::bool_switch(&tiledInput)->default_value(false),           "input data is tiled pdf files")
		("threads",    po::value< uint >(&threadNum)->default_value(0),              "number of threads (automatically detected if 0)")
		("combine",    po::value< bool >(&combineSimilarObjs)->default_value(true),  "combine similar objects (reduces output size but takes considerably longer)")
		("bbox",       po::value< string >(&argClippingBoxStr),                      "clipping bounding box (left,bottom,right,top)");
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
	#ifdef COMPACT_TILE_INDEX
	cout << "tilemaker compiled without 64-bit tile index support, this limits accuracy at very high zoom levels" << endl;
	#endif

	// ---- Check config
	
	if (!boost::filesystem::exists(jsonFile)) { cerr << "Couldn't open .json config: " << jsonFile << endl; return -1; }
	if (!boost::filesystem::exists(luaFile )) { cerr << "Couldn't open .lua script: "  << luaFile  << endl; return -1; }

	bool hasClippingBox = false;
	string clippingBoxSrc;
	Box clippingBox;

	if(!tiledInput)
	{
		// ----	Read bounding box from first .pbf
		double minLon=0.0, maxLon=0.0, minLat=0.0, maxLat=0.0;
		int ret = ReadPbfBoundingBox(inputFiles[0], minLon, maxLon, 
			minLat, maxLat, hasClippingBox);
		if(ret != 0) return ret;
		cout << inputFiles[0] << endl;
		clippingBoxSrc = "first pbf file";
		clippingBox = Box(geom::make<Point>(minLon, minLat),
			              geom::make<Point>(maxLon, maxLat));
	}
	else
	{
		clippingBoxSrc = "tiles on disk";
		hasClippingBox = CheckAvailableDiskTileExtent(inputFiles[0], clippingBox);
	}

	cout << "extent from " << clippingBoxSrc << ":" << clippingBox.min_corner().get<0>() <<","<< clippingBox.min_corner().get<1>() \
		<<","<< clippingBox.max_corner().get<0>() <<","<< clippingBox.max_corner().get<1>() << endl;

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

		bool configHasClippingBox = false;
		Box configClippingBox;
		config.readConfig(jsonConfig, configHasClippingBox, configClippingBox);
		if(configHasClippingBox)
		{
			hasClippingBox = true;
			clippingBox = configClippingBox;
			cout << "using config clipping box " << clippingBox.min_corner().get<0>() <<","<< clippingBox.min_corner().get<1>() \
				<<","<< clippingBox.max_corner().get<0>() <<","<< clippingBox.max_corner().get<1>() << endl;
			clippingBoxSrc = "config file";
		}
	} catch (...) {
		cerr << "Couldn't find expected details in JSON file." << endl;
		return -1;
	}

	/* ----	Command line options override config settings
	 * 
	 * The clipping box is determined, in order of precidence:
	 * 1) command line argument (highest)
	 * 2) json config file setting
	 * 3) extent of first pbf input file
	 */

	stringstream argClippingBoxSs(argClippingBoxStr);
	vector<double> argClippingBox;
	while( argClippingBoxSs.good() )
	{
		string substr;
		getline( argClippingBoxSs, substr, ',' );
		argClippingBox.push_back( atof(substr.c_str()) );
	}
	if(argClippingBox.size() >= 4) //(left,bottom,right,top)
	{
		hasClippingBox = true;
		clippingBox.min_corner().set<0>(argClippingBox[0]);
		clippingBox.max_corner().set<0>(argClippingBox[2]);
		clippingBox.min_corner().set<1>(argClippingBox[1]);
		clippingBox.max_corner().set<1>(argClippingBox[3]);
		clippingBoxSrc = "command line arguments";
	}

	// Copy final clipping box back into config
	if(hasClippingBox)
	{
		config.hasClippingBox = true;
		config.minLon = clippingBox.min_corner().get<0>();
		config.maxLon = clippingBox.max_corner().get<0>();
		config.minLat = clippingBox.min_corner().get<1>();
		config.maxLat = clippingBox.max_corner().get<1>();
	}
	cout << "clipping box source: " << clippingBoxSrc << endl;
	cout << "clipping to " << config.minLon <<","<< config.minLat <<","<< config.maxLon <<","<< config.maxLat << endl;
	if(vm.count("combine")>0)
		config.combineSimilarObjs = combineSimilarObjs;

	// ---- Load external shp files

	class LayerDefinition layers(config.layers);
	class ShpMemTiles shpMemTiles(config.baseZoom);
	for(size_t layerNum=0; layerNum<layers.layers.size(); layerNum++)	
	{
		// External layer sources
		LayerDef &layer = layers.layers[layerNum];
		if(layer.indexed)
			shpMemTiles.CreateNamedLayerIndex(layer.name);

		if (layer.source.size()>0) {
			if (!hasClippingBox) {
				cerr << "Can't read shapefiles unless a bounding box is provided." << endl;
				exit(EXIT_FAILURE);
			}
			readShapefile(clippingBox,
			              layers,
			              config.baseZoom, layerNum,
						  shpMemTiles);
		}
	}

	// For each tile, objects to be used in processing

	shared_ptr<class TileDataSource> osmTiles;
	if(!tiledInput)
		osmTiles.reset(new OsmMemTiles(config.baseZoom,
			inputFiles,
			config,
			luaFile,
			layers,	
			shpMemTiles));
	else
		osmTiles.reset(new OsmDiskTiles(inputFiles[0],
			config,
			luaFile,
			layers,	
			shpMemTiles));

	// ----	Initialise SharedData
	std::vector<class TileDataSource *> sources = {osmTiles.get(), &shpMemTiles};
	class TileData tileData(sources);

	class SharedData sharedData(config, layers, tileData);
	sharedData.threadNum = threadNum;
	sharedData.outputFile = outputFile;
	sharedData.verbose = verbose;
	sharedData.sqlite = sqlite;

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

	// ----	Write out each tile

	// Loop through zoom levels
	for (uint zoom=sharedData.config.startZoom; zoom<=sharedData.config.endZoom; zoom++) {

		tileData.SetZoom(zoom);
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
	}

	// ----	Close tileset

	if (sqlite) {
		// Write mbtiles 1.3+ json object
		sharedData.mbtiles.writeMetadata("json", layers.serialiseToJSON());

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

	cout << endl << "Filled the tileset with good things at " << sharedData.outputFile << endl;
}

