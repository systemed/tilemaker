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
#include <sys/resource.h>
#include <chrono>

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
#include "shp_mem_tiles.h"

// Namespaces
using namespace std;
namespace po = boost::program_options;
namespace geom = boost::geometry;

// Global verbose switch
bool verbose = false;

/**
 *\brief The Main function is responsible for command line processing, loading data and starting worker threads.
 *
 * Data is loaded into OsmMemTiles and ShpMemTiles.
 *
 * Worker threads write the output tiles, and start in the outputProc function.
 */
int main(int argc, char* argv[]) {

	// ----	Read command-line options
	
	vector<string> inputFiles;
	string luaFile;
	string jsonFile;
	uint threadNum;
	string outputFile;
	bool _verbose = false, sqlite= false, combineSimilarObjs = false, mergeSqlite = false, mapsplit = false;

	po::options_description desc("tilemaker (c) 2016-2020 Richard Fairhurst and contributors\nConvert OpenStreetMap .pbf files into vector tiles\n\nAvailable options");
	desc.add_options()
		("help",                                                                 "show help message")
		("input",  po::value< vector<string> >(&inputFiles),                     "source .osm.pbf file")
		("output", po::value< string >(&outputFile),                             "target directory or .mbtiles/.sqlite file")
		("merge"  ,po::bool_switch(&mergeSqlite),                                "merge with existing .mbtiles (overwrites otherwise)")
		("config", po::value< string >(&jsonFile)->default_value("config.json"), "config JSON file")
		("process",po::value< string >(&luaFile)->default_value("process.lua"),  "tag-processing Lua file")
		("verbose",po::bool_switch(&_verbose),                                   "verbose error output")
		("threads",po::value< uint >(&threadNum)->default_value(0),              "number of threads (automatically detected if 0)")
		("combine",po::bool_switch(&combineSimilarObjs),                         "combine similar objects (reduces output size but takes considerably longer)");
	po::positional_options_description p;
	p.add("input", -1);
	po::variables_map vm;
    try {
        po::store(po::command_line_parser(argc, argv).options(desc).positional(p).run(), vm);
    } catch (const po::unknown_option& ex) {
        cerr << "Unknown option: " << ex.get_option_name() << endl;
        return -1;
    }
	po::notify(vm);
	
	if (vm.count("help")) { cout << desc << endl; return 0; }
	if (vm.count("output")==0) { cerr << "You must specify an output file or directory. Run with --help to find out more." << endl; return -1; }
	if (vm.count("input")==0) { cout << "No source .osm.pbf file supplied" << endl; }

	if (ends_with(outputFile, ".mbtiles") || ends_with(outputFile, ".sqlite")) { sqlite=true; }
	if (threadNum == 0) { threadNum = max(thread::hardware_concurrency(), 1u); }
	verbose = _verbose;

	#ifdef COMPACT_NODES
	cout << "tilemaker compiled without 64-bit node support, use 'osmium renumber' first if working with OpenStreetMap-sourced data" << endl;
	#endif

	// ---- Check config
	
	if (!boost::filesystem::exists(jsonFile)) { cerr << "Couldn't open .json config: " << jsonFile << endl; return -1; }
	if (!boost::filesystem::exists(luaFile )) { cerr << "Couldn't open .lua script: "  << luaFile  << endl; return -1; }

	// ---- Remove existing .mbtiles if it exists

	if (sqlite && !mergeSqlite && static_cast<bool>(std::ifstream(outputFile))) {
		cout << "mbtiles file exists, will overwrite (Ctrl-C to abort, rerun with --merge to keep)" << endl;
		std::this_thread::sleep_for(std::chrono::milliseconds(2000));
		if (remove(outputFile.c_str()) != 0) {
			cerr << "Couldn't remove existing file" << endl;
			return 0;
		}
	}

	// ----	Read bounding box from first .pbf (if there is one) or mapsplit file

	bool hasClippingBox = false;
	Box clippingBox;
	MBTiles mapsplitFile;
	double minLon=0.0, maxLon=0.0, minLat=0.0, maxLat=0.0;
	if (inputFiles.size()==1 && (ends_with(inputFiles[0], ".mbtiles") || ends_with(inputFiles[0], ".sqlite") || ends_with(inputFiles[0], ".msf"))) {
		mapsplit = true;
		mapsplitFile.openForReading(&inputFiles[0]);
		mapsplitFile.readBoundingBox(minLon, maxLon, minLat, maxLat);
		cout << "Bounding box " << minLon << ", " << maxLon << ", " << minLat << ", " << maxLat << endl;
		clippingBox = Box(geom::make<Point>(minLon, lat2latp(minLat)),
		                  geom::make<Point>(maxLon, lat2latp(maxLat)));
		hasClippingBox = true;

	} else if (inputFiles.size()>0) {
		int ret = ReadPbfBoundingBox(inputFiles[0], minLon, maxLon, minLat, maxLat, hasClippingBox);
		if(ret != 0) return ret;
		if(hasClippingBox) {
			clippingBox = Box(geom::make<Point>(minLon, lat2latp(minLat)),
			                  geom::make<Point>(maxLon, lat2latp(maxLat)));
		}
	}

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
		config.combineSimilarObjs = combineSimilarObjs;
	} catch (...) {
		cerr << "Couldn't find expected details in JSON file." << endl;
		return -1;
	}

	// For each tile, objects to be used in processing
	class OsmMemTiles osmMemTiles(config.baseZoom);
	class ShpMemTiles shpMemTiles(config.baseZoom);
	class LayerDefinition layers(config.layers);
	OsmLuaProcessing osmLuaProcessing(config, layers, luaFile, 
		shpMemTiles, 
		osmMemTiles);

	// ---- Load external shp files

	for (size_t layerNum=0; layerNum<layers.layers.size(); layerNum++) {
		// External layer sources
		LayerDef &layer = layers.layers[layerNum];
		if(layer.indexed) { shpMemTiles.CreateNamedLayerIndex(layer.name); }

		if (layer.source.size()>0) {
			if (!hasClippingBox) {
				cerr << "Can't read shapefiles unless a bounding box is provided." << endl;
				exit(EXIT_FAILURE);
			}
			cout << "Reading .shp " << layer.name << endl;
			readShapefile(clippingBox,
			              layers,
			              config.baseZoom, layerNum,
						  shpMemTiles, osmLuaProcessing);
		}
	}

	// ----	Read significant node tags

	vector<string> nodeKeyVec = osmLuaProcessing.GetSignificantNodeKeys();
	unordered_set<string> nodeKeys(nodeKeyVec.begin(), nodeKeyVec.end());

	// ----	Read all PBFs
	
	class PbfReader pbfReader;
	pbfReader.output = &osmLuaProcessing;
	if (!mapsplit) {
		for (auto inputFile : inputFiles) {
			cout << "Reading .pbf " << inputFile << endl;
			int ret = pbfReader.ReadPbfFile(inputFile, nodeKeys);
			if (ret != 0) return ret;
		}
	}

	// ----	Initialise SharedData

	std::vector<class TileDataSource *> sources = {&osmMemTiles, &shpMemTiles};
	class TileData tileData(sources);

	class SharedData sharedData(config, layers, tileData);
	sharedData.threadNum = threadNum;
	sharedData.outputFile = outputFile;
	sharedData.sqlite = sqlite;

	// ----	Initialise mbtiles if required
	
	if (sharedData.sqlite) {
		ostringstream bounds;
		bounds << fixed << sharedData.config.minLon << "," << sharedData.config.minLat << "," << sharedData.config.maxLon << "," << sharedData.config.maxLat;
		sharedData.mbtiles.openForWriting(&sharedData.outputFile);
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

	// ----	Write out data

	// If mapsplit, read list of tiles available
	unsigned runs=1;
	vector<tuple<int,int,int>> tileList;
	if (mapsplit) {
		mapsplitFile.readTileList(tileList);
		runs = tileList.size();
	}

	for (unsigned run=0; run<runs; run++) {
		// Read mapsplit tile and parse, if applicable
		int srcZ = -1, srcX = -1, srcY = -1, tmsY = -1;
		if (mapsplit) {
			osmMemTiles.Clear();
			tie(srcZ,srcX,tmsY) = tileList.back();
			srcY = pow(2,srcZ) - tmsY - 1; // TMS
			cout << "Reading tile " << srcZ << ": " << srcX << "," << srcY << " (" << (run+1) << "/" << runs << ")" << endl;
			vector<char> pbf = mapsplitFile.readTile(srcZ,srcX,tmsY);
			// Write to a temp file and read back in - can we do this better via a stringstream or similar?
			ofstream tempfile;
			tempfile.open("/tmp/temp.pbf");
			copy(pbf.cbegin(), pbf.cend(), ostreambuf_iterator<char>(tempfile));
			tempfile.close();
			pbfReader.ReadPbfFile("/tmp/temp.pbf", nodeKeys);
			tileList.pop_back();
		}

		for (uint zoom=sharedData.config.startZoom; zoom<=sharedData.config.endZoom; zoom++) {

			tileData.SetZoom(zoom);
			sharedData.zoom = zoom;

			if(threadNum == 1) {
				// Single thread (is easier to debug)
				outputProc(0, &sharedData, srcZ,srcX,srcY);
			} else {

				// Multi thread processing loop
				vector<thread> worker;
				for (uint threadId = 0; threadId < threadNum; threadId++)
					worker.emplace_back(outputProc, threadId, &sharedData, srcZ,srcX,srcY);
				for (auto &t: worker) t.join();

			}
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
		sharedData.mbtiles.closeForWriting();
	}
	google::protobuf::ShutdownProtobufLibrary();

	if (verbose) {
		struct rusage r_usage;
		getrusage(RUSAGE_SELF, &r_usage);
		cout << "\nMemory used: " << r_usage.ru_maxrss << endl;
	}

	cout << endl << "Filled the tileset with good things at " << sharedData.outputFile << endl;
}

