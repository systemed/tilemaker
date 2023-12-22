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
#include <chrono>

// Other utilities
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/program_options.hpp>
#include <boost/variant.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/sort/sort.hpp>

#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/filereadstream.h"
#include "rapidjson/filewritestream.h"

#ifndef _MSC_VER
#include <sys/resource.h>
#endif

#include "geom.h"
#include "node_stores.h"
#include "way_stores.h"

// Tilemaker code
#include "helpers.h"
#include "coordinates.h"
#include "coordinates_geom.h"

#include "attribute_store.h"
#include "output_object.h"
#include "osm_lua_processing.h"
#include "mbtiles.h"

#include "shared_data.h"
#include "read_pbf.h"
#include "read_shp.h"
#include "tile_worker.h"
#include "osm_mem_tiles.h"
#include "shp_mem_tiles.h"

#include <boost/asio/post.hpp>
#include <boost/interprocess/streams/bufferstream.hpp>

#ifndef TM_VERSION
#define TM_VERSION (version not set)
#endif
#define STR1(x)  #x
#define STR(x)  STR1(x)

// Namespaces
using namespace std;
namespace po = boost::program_options;
namespace geom = boost::geometry;

// Global verbose switch
bool verbose = false;

void WriteSqliteMetadata(rapidjson::Document const &jsonConfig, SharedData &sharedData, LayerDefinition const &layers)
{
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

void WriteFileMetadata(rapidjson::Document const &jsonConfig, SharedData const &sharedData, LayerDefinition const &layers)
{
	if(sharedData.config.compress) 
		std::cout << "When serving compressed tiles, make sure to include 'Content-Encoding: gzip' in your webserver configuration for serving pbf files"  << std::endl;

	rapidjson::Document document;
	document.SetObject();

	if (jsonConfig["settings"].HasMember("filemetadata")) {
		const rapidjson::Value &md = jsonConfig["settings"]["filemetadata"];
		document.CopyFrom(md, document.GetAllocator());
	}

	rapidjson::Value boundsArray(rapidjson::kArrayType);
	boundsArray.PushBack(rapidjson::Value(sharedData.config.minLon), document.GetAllocator());
	boundsArray.PushBack(rapidjson::Value(sharedData.config.minLat), document.GetAllocator());
	boundsArray.PushBack(rapidjson::Value(sharedData.config.maxLon), document.GetAllocator());
	boundsArray.PushBack(rapidjson::Value(sharedData.config.maxLat), document.GetAllocator());
	document.AddMember("bounds", boundsArray, document.GetAllocator());

	document.AddMember("name", rapidjson::Value().SetString(sharedData.config.projectName.c_str(), document.GetAllocator()), document.GetAllocator());
	document.AddMember("version", rapidjson::Value().SetString(sharedData.config.projectVersion.c_str(), document.GetAllocator()), document.GetAllocator());
	document.AddMember("description", rapidjson::Value().SetString(sharedData.config.projectDesc.c_str(), document.GetAllocator()), document.GetAllocator());
	document.AddMember("minzoom", rapidjson::Value(sharedData.config.startZoom), document.GetAllocator());
	document.AddMember("maxzoom", rapidjson::Value(sharedData.config.endZoom), document.GetAllocator());
	document.AddMember("vector_layers", layers.serialiseToJSONValue(document.GetAllocator()), document.GetAllocator());

	auto fp = std::fopen((sharedData.outputFile + "/metadata.json").c_str(), "w");

	char writeBuffer[65536];
	rapidjson::FileWriteStream os(fp, writeBuffer, sizeof(writeBuffer));
	rapidjson::Writer<rapidjson::FileWriteStream> writer(os);
	document.Accept(writer);

	fclose(fp);
}

double bboxElementFromStr(const string& number) {
	try {
		return boost::lexical_cast<double>(number);
	} catch (boost::bad_lexical_cast&) {
		cerr << "Failed to parse coordinate " << number << endl;
		exit(1);
	}
}

/**
 * Split bounding box provided as a comma-separated list of coordinates.
 */
vector<string> parseBox(const string& bbox) {
	vector<string> bboxParts;
	if (!bbox.empty()) {
		boost::split(bboxParts, bbox, boost::is_any_of(","));
		if (bboxParts.size() != 4) {
			cerr << "Bounding box must contain 4 elements: minlon,minlat,maxlon,maxlat" << endl;
			exit(1);
		}
	}
	return bboxParts;
}

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
	string osmStoreFile;
	string jsonFile;
	uint threadNum;
	string outputFile;
	string bbox;
	bool _verbose = false, sqlite= false, mergeSqlite = false, mapsplit = false, osmStoreCompact = false, skipIntegrity = false, osmStoreUncompressedNodes = false, osmStoreUncompressedWays = false, materializeGeometries = false, shardStores = false;
	bool logTileTimings = false;

	po::options_description desc("tilemaker " STR(TM_VERSION) "\nConvert OpenStreetMap .pbf files into vector tiles\n\nAvailable options");
	desc.add_options()
		("help",                                                                 "show help message")
		("input",  po::value< vector<string> >(&inputFiles),                     "source .osm.pbf file")
		("output", po::value< string >(&outputFile),                             "target directory or .mbtiles/.sqlite file")
		("bbox",   po::value< string >(&bbox),                                   "bounding box to use if input file does not have a bbox header set, example: minlon,minlat,maxlon,maxlat")
		("merge"  ,po::bool_switch(&mergeSqlite),                                "merge with existing .mbtiles (overwrites otherwise)")
		("config", po::value< string >(&jsonFile)->default_value("config.json"), "config JSON file")
		("process",po::value< string >(&luaFile)->default_value("process.lua"),  "tag-processing Lua file")
		("store",  po::value< string >(&osmStoreFile),  "temporary storage for node/ways/relations data")
		("compact",po::bool_switch(&osmStoreCompact),  "Reduce overall memory usage (compact mode).\nNOTE: This requires the input to be renumbered (osmium renumber)")
		("no-compress-nodes", po::bool_switch(&osmStoreUncompressedNodes),  "Store nodes uncompressed")
		("no-compress-ways", po::bool_switch(&osmStoreUncompressedWays),  "Store ways uncompressed")
		("materialize-geometries", po::bool_switch(&materializeGeometries),  "Materialize geometries - faster, but requires more memory")
		("shard-stores", po::bool_switch(&shardStores),  "Shard stores - use an alternate reading/writing strategy for low-memory machines")
		("verbose",po::bool_switch(&_verbose),                                   "verbose error output")
		("skip-integrity",po::bool_switch(&skipIntegrity),                       "don't enforce way/node integrity")
		("log-tile-timings", po::bool_switch(&logTileTimings), "log how long each tile takes")
		("threads",po::value< uint >(&threadNum)->default_value(0),              "number of threads (automatically detected if 0)");
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

	vector<string> bboxElements = parseBox(bbox);

	if (ends_with(outputFile, ".mbtiles") || ends_with(outputFile, ".sqlite")) { sqlite=true; }
	if (threadNum == 0) { threadNum = max(thread::hardware_concurrency(), 1u); }
	verbose = _verbose;


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
	} else if (mergeSqlite && !static_cast<bool>(std::ifstream(outputFile))) {
		cout << "--merge specified but .mbtiles file doesn't already exist, ignoring" << endl;
		mergeSqlite = false;
	}

	// ----	Read bounding box from first .pbf (if there is one) or mapsplit file

	bool hasClippingBox = false;
	Box clippingBox;
	MBTiles mapsplitFile;
	double minLon=0.0, maxLon=0.0, minLat=0.0, maxLat=0.0;
	if (!bboxElements.empty()) {
		hasClippingBox = true;
		minLon = bboxElementFromStr(bboxElements.at(0));
		minLat = bboxElementFromStr(bboxElements.at(1));
		maxLon = bboxElementFromStr(bboxElements.at(2));
		maxLat = bboxElementFromStr(bboxElements.at(3));

	} else if (inputFiles.size()==1 && (ends_with(inputFiles[0], ".mbtiles") || ends_with(inputFiles[0], ".sqlite") || ends_with(inputFiles[0], ".msf"))) {
		mapsplit = true;
		mapsplitFile.openForReading(inputFiles[0]);
		mapsplitFile.readBoundingBox(minLon, maxLon, minLat, maxLat);
		hasClippingBox = true;

	} else if (inputFiles.size()>0) {
		int ret = ReadPbfBoundingBox(inputFiles[0], minLon, maxLon, minLat, maxLat, hasClippingBox);
		if(ret != 0) return ret;
	}

	if (hasClippingBox) {
		clippingBox = Box(geom::make<Point>(minLon, lat2latp(minLat)),
		                  geom::make<Point>(maxLon, lat2latp(maxLat)));
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
	} catch (...) {
		cerr << "Couldn't find expected details in JSON file." << endl;
		return -1;
	}
	if (hasClippingBox) {
		cout << "Bounding box " << clippingBox.min_corner().x() << ", " << latp2lat(clippingBox.min_corner().y()) << ", " << 
		                           clippingBox.max_corner().x() << ", " << latp2lat(clippingBox.max_corner().y()) << endl;
	}

	// For each tile, objects to be used in processing
	bool allPbfsHaveSortTypeThenID = true;
	bool anyPbfHasLocationsOnWays = false;

	for (const std::string& file: inputFiles) {
		if (ends_with(file, ".pbf")) {
			allPbfsHaveSortTypeThenID = allPbfsHaveSortTypeThenID && PbfHasOptionalFeature(file, OptionSortTypeThenID);
			anyPbfHasLocationsOnWays = anyPbfHasLocationsOnWays || PbfHasOptionalFeature(file, OptionLocationsOnWays);
		}
	}

	auto createNodeStore = [allPbfsHaveSortTypeThenID, osmStoreCompact, osmStoreUncompressedNodes]() {
		if (osmStoreCompact) {
			std::shared_ptr<NodeStore> rv = make_shared<CompactNodeStore>();
			return rv;
		}

		if (allPbfsHaveSortTypeThenID) {
			std::shared_ptr<NodeStore> rv = make_shared<SortedNodeStore>(!osmStoreUncompressedNodes);
			return rv;
		}
		std::shared_ptr<NodeStore> rv =  make_shared<BinarySearchNodeStore>();
		return rv;
	};

	shared_ptr<NodeStore> nodeStore;

	if (shardStores) {
		nodeStore = std::make_shared<ShardedNodeStore>(createNodeStore);
	} else {
		nodeStore = createNodeStore();
	}

	auto createWayStore = [anyPbfHasLocationsOnWays, allPbfsHaveSortTypeThenID, osmStoreUncompressedWays, &nodeStore]() {
		if (!anyPbfHasLocationsOnWays && allPbfsHaveSortTypeThenID) {
			std::shared_ptr<WayStore> rv = make_shared<SortedWayStore>(!osmStoreUncompressedWays, *nodeStore.get());
			return rv;
		}

		std::shared_ptr<WayStore> rv = make_shared<BinarySearchWayStore>();
		return rv;
	};

	shared_ptr<WayStore> wayStore;
	if (shardStores) {
		wayStore = std::make_shared<ShardedWayStore>(createWayStore, *nodeStore.get());
	} else {
		wayStore = createWayStore();
	}

	OSMStore osmStore(*nodeStore.get(), *wayStore.get());
	osmStore.use_compact_store(osmStoreCompact);
	osmStore.enforce_integrity(!skipIntegrity);
	if(!osmStoreFile.empty()) {
		std::cout << "Using osm store file: " << osmStoreFile << std::endl;
		osmStore.open(osmStoreFile);
	}

	AttributeStore attributeStore;

	class LayerDefinition layers(config.layers);
	class OsmMemTiles osmMemTiles(threadNum, config.baseZoom, config.includeID, *nodeStore, *wayStore);
	class ShpMemTiles shpMemTiles(threadNum, config.baseZoom);
	osmMemTiles.open();
	shpMemTiles.open();

	OsmLuaProcessing osmLuaProcessing(osmStore, config, layers, luaFile, 
		shpMemTiles, osmMemTiles, attributeStore, materializeGeometries);

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
			              threadNum,
			              shpMemTiles, osmLuaProcessing);
		}
	}
	shpMemTiles.reportSize();

	// ----	Read significant node tags

	vector<string> nodeKeyVec = osmLuaProcessing.GetSignificantNodeKeys();
	unordered_set<string> nodeKeys(nodeKeyVec.begin(), nodeKeyVec.end());

	// ----	Read all PBFs
	
	PbfProcessor pbfProcessor(osmStore);
	std::vector<bool> sortOrders = layers.getSortOrders();

	if (!mapsplit) {
		for (auto inputFile : inputFiles) {
			cout << "Reading .pbf " << inputFile << endl;
			ifstream infile(inputFile, ios::in | ios::binary);
			if (!infile) { cerr << "Couldn't open .pbf file " << inputFile << endl; return -1; }
			
			const bool hasSortTypeThenID = PbfHasOptionalFeature(inputFile, OptionSortTypeThenID);
			int ret = pbfProcessor.ReadPbfFile(
				nodeStore->shards(),
				hasSortTypeThenID,
				nodeKeys,
				threadNum,
				[&]() {
					thread_local std::shared_ptr<ifstream> pbfStream(new ifstream(inputFile, ios::in | ios::binary));
					return pbfStream;
				},
				[&]() {
					thread_local std::shared_ptr<OsmLuaProcessing> osmLuaProcessing(new OsmLuaProcessing(osmStore, config, layers, luaFile, shpMemTiles, osmMemTiles, attributeStore, materializeGeometries));
					return osmLuaProcessing;
				},
				*nodeStore,
				*wayStore
			);
			if (ret != 0) return ret;
		} 
		attributeStore.finalize();
		osmMemTiles.reportSize();
		attributeStore.reportSize();
	}
	// ----	Initialise SharedData
	SourceList sources = {&osmMemTiles, &shpMemTiles};
	class SharedData sharedData(config, layers);
	sharedData.outputFile = outputFile;
	sharedData.sqlite = sqlite;
	sharedData.mergeSqlite = mergeSqlite;

	// ----	Initialise mbtiles if required
	
	if (sharedData.sqlite) {
		sharedData.mbtiles.openForWriting(sharedData.outputFile);
		sharedData.mbtiles.writeMetadata("name",sharedData.config.projectName);
		sharedData.mbtiles.writeMetadata("type","baselayer");
		sharedData.mbtiles.writeMetadata("version",sharedData.config.projectVersion);
		sharedData.mbtiles.writeMetadata("description",sharedData.config.projectDesc);
		sharedData.mbtiles.writeMetadata("format","pbf");
		sharedData.mbtiles.writeMetadata("minzoom",to_string(sharedData.config.startZoom));
		sharedData.mbtiles.writeMetadata("maxzoom",to_string(sharedData.config.endZoom));

		ostringstream bounds;
		if (mergeSqlite) {
			double cMinLon, cMaxLon, cMinLat, cMaxLat;
			sharedData.mbtiles.readBoundingBox(cMinLon, cMaxLon, cMinLat, cMaxLat);
			sharedData.config.enlargeBbox(cMinLon, cMaxLon, cMinLat, cMaxLat);
		}
		bounds << fixed << sharedData.config.minLon << "," << sharedData.config.minLat << "," << sharedData.config.maxLon << "," << sharedData.config.maxLat;
		sharedData.mbtiles.writeMetadata("bounds",bounds.str());

		if (!sharedData.config.defaultView.empty()) {
			sharedData.mbtiles.writeMetadata("center",sharedData.config.defaultView);
		} else {
			double centerLon = (sharedData.config.minLon + sharedData.config.maxLon) / 2;
			double centerLat = (sharedData.config.minLat + sharedData.config.maxLat) / 2;
			int centerZoom = floor((sharedData.config.startZoom + sharedData.config.endZoom) / 2);
			ostringstream center;
			center << fixed << centerLon << "," << centerLat << "," << centerZoom;
			sharedData.mbtiles.writeMetadata("center",center.str());
		}
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
			if (srcZ > config.baseZoom) {
				cerr << "Mapsplit tiles (zoom " << srcZ << ") must not be greater than basezoom " << config.baseZoom << endl;
				return 0;
			} else if (srcZ > config.startZoom) {
				cout << "Mapsplit tiles (zoom " << srcZ << ") can't write data at zoom level " << config.startZoom << endl;
			}

			cout << "Reading tile " << srcZ << ": " << srcX << "," << srcY << " (" << (run+1) << "/" << runs << ")" << endl;
			vector<char> pbf = mapsplitFile.readTile(srcZ,srcX,tmsY);

			int ret = pbfProcessor.ReadPbfFile(
				nodeStore->shards(),
				false,
				nodeKeys,
				1,
				[&]() {
					return make_unique<boost::interprocess::bufferstream>(pbf.data(), pbf.size(),  ios::in | ios::binary);
				},
				[&]() {
					return std::make_unique<OsmLuaProcessing>(osmStore, config, layers, luaFile, shpMemTiles, osmMemTiles, attributeStore, materializeGeometries);
				},
				*nodeStore,
				*wayStore
			);
			if (ret != 0) return ret;

			tileList.pop_back();
		}

		// Launch the pool with threadNum threads
		boost::asio::thread_pool pool(threadNum);

		// Mutex is hold when IO is performed
		std::mutex io_mutex;

		// Loop through tiles
		std::atomic<uint64_t> tilesWritten(0);

		for (auto source : sources) {
			source->finalize(threadNum);
		}
		// tiles by zoom level

		// The clipping bbox check is expensive - as an optimization, compute the set of
		// z6 tiles that are wholly covered by the clipping box. Membership in this
		// set is quick to test.
		TileCoordinatesSet coveredZ6Tiles(6);
		if (hasClippingBox) {
			for (int x = 0; x < 1 << 6; x++) {
				for (int y = 0; y < 1 << 6; y++) {
					if (boost::geometry::within(
								TileBbox(TileCoordinates(x, y), 6, false, false).getTileBox(),
								clippingBox
							))
						coveredZ6Tiles.set(x, y);
				}
			}
		}

		std::deque<std::pair<unsigned int, TileCoordinates>> tileCoordinates;
		std::vector<TileCoordinatesSet> zoomResults;
		for (uint zoom = 0; zoom <= sharedData.config.endZoom; zoom++) {
			zoomResults.push_back(TileCoordinatesSet(zoom));
		}

		{
#ifdef CLOCK_MONOTONIC
			timespec start, end;
			clock_gettime(CLOCK_MONOTONIC, &start);
#endif
			std::cout << "collecting tiles" << std::flush;
			populateTilesAtZoom(sources, zoomResults);
#ifdef CLOCK_MONOTONIC
			clock_gettime(CLOCK_MONOTONIC, &end);
			uint64_t tileNs = 1e9 * (end.tv_sec - start.tv_sec) + end.tv_nsec - start.tv_nsec;
			std::cout << ": " << (uint32_t)(tileNs / 1e6) << "ms";
#endif
		}

		std::cout << ", filtering tiles:" << std::flush;
		for (uint zoom=sharedData.config.startZoom; zoom <= sharedData.config.endZoom; zoom++) {
			std::cout << " z" << std::to_string(zoom) << std::flush;
#ifdef CLOCK_MONOTONIC
			timespec start, end;
			clock_gettime(CLOCK_MONOTONIC, &start);
#endif

			const auto& zoomResult = zoomResults[zoom];
			int numTiles = 0;
			for (int x = 0; x < 1 << zoom; x++) {
				for (int y = 0; y < 1 << zoom; y++) {
					if (!zoomResult.test(x, y))
						continue;

					// If we're constrained to a source tile, check we're within it
					if (srcZ > -1) {
						int xAtSrcZ = x / pow(2, zoom-srcZ);
						int yAtSrcZ = y / pow(2, zoom-srcZ);
						if (xAtSrcZ != srcX || yAtSrcZ != srcY) continue;
					}
				
					if (hasClippingBox) {
						bool isInAWhollyCoveredZ6Tile = false;
						if (zoom >= 6) {
							TileCoordinate z6x = x / (1 << (zoom - 6));
							TileCoordinate z6y = y / (1 << (zoom - 6));
							isInAWhollyCoveredZ6Tile = coveredZ6Tiles.test(z6x, z6y);
						}

						if(!isInAWhollyCoveredZ6Tile && !boost::geometry::intersects(TileBbox(TileCoordinates(x, y), zoom, false, false).getTileBox(), clippingBox)) 
							continue;
					}

					tileCoordinates.push_back(std::make_pair(zoom, TileCoordinates(x, y)));
					numTiles++;
				}
			}

			std::cout << " (" << numTiles;
#ifdef CLOCK_MONOTONIC
			clock_gettime(CLOCK_MONOTONIC, &end);
			uint64_t tileNs = 1e9 * (end.tv_sec - start.tv_sec) + end.tv_nsec - start.tv_nsec;
			std::cout << ", " << (uint32_t)(tileNs / 1e6) << "ms";

#endif
			std::cout << ")" << std::flush;
		}
		zoomResults.clear();

		std::cout << std::endl;

		// Cluster tiles: breadth-first for z0..z5, depth-first for z6
		const size_t baseZoom = config.baseZoom;
		boost::sort::block_indirect_sort(
			tileCoordinates.begin(), tileCoordinates.end(), 
			[baseZoom](auto const &a, auto const &b) {
				const auto aZoom = a.first;
				const auto bZoom = b.first;
				const auto aX = a.second.x;
				const auto aY = a.second.y;
				const auto bX = b.second.x;
				const auto bY = b.second.y;
				const bool aLowZoom = aZoom < CLUSTER_ZOOM;
				const bool bLowZoom = bZoom < CLUSTER_ZOOM;

				// Breadth-first for z0..5
				if (aLowZoom != bLowZoom)
					return aLowZoom;

				if (aLowZoom && bLowZoom) {
					if (aZoom != bZoom)
						return aZoom < bZoom;

					if (aX != bX)
						return aX < bX;

					return aY < bY;
				}

				for (size_t z = CLUSTER_ZOOM; z <= baseZoom; z++) {
					// Translate both a and b to zoom z, compare.
					// First, sanity check: can we translate it to this zoom?
					if (aZoom < z || bZoom < z) {
						return aZoom < bZoom;
					}

					const auto aXz = aX / (1 << (aZoom - z));
					const auto aYz = aY / (1 << (aZoom - z));
					const auto bXz = bX / (1 << (bZoom - z));
					const auto bYz = bY / (1 << (bZoom - z));

					if (aXz != bXz)
						return aXz < bXz;

					if (aYz != bYz)
						return aYz < bYz;
				}

				return false;
			}, 
			threadNum);

		std::size_t batchSize = 0;
		for(std::size_t startIndex = 0; startIndex < tileCoordinates.size(); startIndex += batchSize) {
			// Compute how many tiles should be assigned to this batch --
			// higher-zoom tiles are cheaper to compute, lower-zoom tiles more expensive.
			batchSize = 0;
			size_t weight = 0;
			while (weight < 1000 && startIndex + batchSize < tileCoordinates.size()) {
				const auto& zoom = tileCoordinates[startIndex + batchSize].first;
				if (zoom > 12)
					weight++;
				else if (zoom > 11)
					weight += 10;
				else if (zoom > 10)
					weight += 100;
				else
					weight += 1000;

				batchSize++;
			}

			boost::asio::post(pool, [=, &tileCoordinates, &pool, &sharedData, &sources, &attributeStore, &io_mutex, &tilesWritten]() {
				std::vector<std::string> tileTimings;
				std::size_t endIndex = std::min(tileCoordinates.size(), startIndex + batchSize);
				for(std::size_t i = startIndex; i < endIndex; ++i) {
					unsigned int zoom = tileCoordinates[i].first;
					TileCoordinates coords = tileCoordinates[i].second;

#ifdef CLOCK_MONOTONIC
					timespec start, end;
					if (logTileTimings)
						clock_gettime(CLOCK_MONOTONIC, &start);
#endif

					std::vector<std::vector<OutputObjectID>> data;
					for (auto source : sources) {
						data.emplace_back(source->getObjectsForTile(sortOrders, zoom, coords));
					}
					outputProc(sharedData, sources, attributeStore, data, coords, zoom);

#ifdef CLOCK_MONOTONIC
					if (logTileTimings) {
						clock_gettime(CLOCK_MONOTONIC, &end);
						uint64_t tileNs = 1e9 * (end.tv_sec - start.tv_sec) + end.tv_nsec - start.tv_nsec;
						std::string output = "z" + std::to_string(zoom) + "/" + std::to_string(coords.x) + "/" + std::to_string(coords.y) + " took " + std::to_string(tileNs/1e6) + " ms";
						tileTimings.push_back(output);
					}
#endif
				}

				if (logTileTimings) {
					const std::lock_guard<std::mutex> lock(io_mutex);
					std::cout << std::endl;
					for (const auto& output : tileTimings)
						std::cout << output << std::endl;
				}

				tilesWritten += (endIndex - startIndex); 

				if (io_mutex.try_lock()) {
					// Show progress grouped by z6 (or lower)
					size_t z = tileCoordinates[startIndex].first;
					size_t x = tileCoordinates[startIndex].second.x;
					size_t y = tileCoordinates[startIndex].second.y;
					if (z > CLUSTER_ZOOM) {
						x = x / (1 << (z - CLUSTER_ZOOM));
						y = y / (1 << (z - CLUSTER_ZOOM));
						z = CLUSTER_ZOOM;
					}
					cout << "z" << z << "/" << x << "/" << y << ", writing tile " << tilesWritten.load() << " of " << tileCoordinates.size() << "               \r" << std::flush;
					io_mutex.unlock();
				}
			});
		}
		// Wait for all tasks in the pool to complete.
		pool.join();
	}

	// ----	Close tileset

	if (sqlite)
		WriteSqliteMetadata(jsonConfig, sharedData, layers);
	else 
		WriteFileMetadata(jsonConfig, sharedData, layers);

	google::protobuf::ShutdownProtobufLibrary();

#ifndef _MSC_VER
	if (verbose) {
		struct rusage r_usage;
		getrusage(RUSAGE_SELF, &r_usage);
		cout << "\nMemory used: " << r_usage.ru_maxrss << endl;
	}
#endif

	cout << endl << "Filled the tileset with good things at " << sharedData.outputFile << endl;
	void_mmap_allocator::shutdown(); // this clears the mmap'ed nodes/ways/relations (quickly!)
}

