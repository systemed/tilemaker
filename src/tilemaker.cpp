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
#include "significant_tags.h"

#include "attribute_store.h"
#include "output_object.h"
#include "osm_lua_processing.h"
#include "mbtiles.h"

#include "options_parser.h"
#include "shared_data.h"
#include "pbf_processor.h"
#include "geojson_processor.h"
#include "shp_processor.h"
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


/**
 *\brief The Main function is responsible for command line processing, loading data and starting worker threads.
 *
 * Data is loaded into OsmMemTiles and ShpMemTiles.
 *
 * Worker threads write the output tiles, and start in the outputProc function.
 */
int main(const int argc, const char* argv[]) {
	// ----	Read command-line options
	OptionsParser::Options options;
	try {
		options = OptionsParser::parse(argc, argv);
	} catch (OptionsParser::OptionException& e) {
		cerr << e.what() << endl;
		return 1;
	}

	if (options.showHelp) { OptionsParser::showHelp(); return 0; }

	if (options.quiet) {
		// Suppress anything written to std out
		std::cout.setstate(std::ios_base::failbit);
	}

	verbose = options.verbose;

	vector<string> bboxElements = parseBox(options.bbox);

	// ---- Remove existing .mbtiles if it exists
	if ((options.outputMode == OptionsParser::OutputMode::MBTiles || options.outputMode == OptionsParser::OutputMode::PMTiles) && !options.mergeSqlite && static_cast<bool>(std::ifstream(options.outputFile))) {
		cout << "Output file exists, will overwrite (Ctrl-C to abort";
		if (options.outputMode == OptionsParser::OutputMode::MBTiles) cout << ", rerun with --merge to keep";
		cout << ")" << endl;
		std::this_thread::sleep_for(std::chrono::milliseconds(2000));
		if (remove(options.outputFile.c_str()) != 0) {
			cerr << "Couldn't remove existing file" << endl;
			return 0;
		}
	} else if (options.mergeSqlite && options.outputMode != OptionsParser::OutputMode::MBTiles) {
		cerr << "--merge only works with .mbtiles" << endl;
		return 0;
	} else if (options.mergeSqlite && !static_cast<bool>(std::ifstream(options.outputFile))) {
		cout << "--merge specified but .mbtiles file doesn't already exist, ignoring" << endl;
		options.mergeSqlite = false;
	}


	// ----	Read bounding box from first .pbf (if there is one)

	bool hasClippingBox = false;
	Box clippingBox;
	double minLon=std::numeric_limits<double>::max(),
				 maxLon=std::numeric_limits<double>::min(),
				 minLat=std::numeric_limits<double>::max(),
				 maxLat=std::numeric_limits<double>::min();
	if (!bboxElements.empty()) {
		hasClippingBox = true;
		minLon = bboxElementFromStr(bboxElements.at(0));
		minLat = bboxElementFromStr(bboxElements.at(1));
		maxLon = bboxElementFromStr(bboxElements.at(2));
		maxLat = bboxElementFromStr(bboxElements.at(3));

	} else if (options.inputFiles.size()>0) {
		for (const auto inputFile : options.inputFiles) {
			bool localHasClippingBox;
			double localMinLon, localMaxLon, localMinLat, localMaxLat;
			int ret = ReadPbfBoundingBox(inputFile, localMinLon, localMaxLon, localMinLat, localMaxLat, localHasClippingBox);
			if(ret != 0) return ret;
			hasClippingBox = hasClippingBox || localHasClippingBox;

			if (localHasClippingBox) {
				minLon = std::min(minLon, localMinLon);
				maxLon = std::max(maxLon, localMaxLon);
				minLat = std::min(minLat, localMinLat);
				maxLat = std::max(maxLat, localMaxLat);
			}
		}
	}

	if (hasClippingBox) {
		clippingBox = Box(geom::make<Point>(minLon, lat2latp(minLat)),
		                  geom::make<Point>(maxLon, lat2latp(maxLat)));
	}

	// ----	Read JSON config

	rapidjson::Document jsonConfig;
	class Config config;
	try {
		FILE* fp = fopen(options.jsonFile.c_str(), "r");
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

	for (const std::string& file: options.inputFiles) {
		if (ends_with(file, ".pbf")) {
			allPbfsHaveSortTypeThenID = allPbfsHaveSortTypeThenID && PbfHasOptionalFeature(file, OptionSortTypeThenID);
			anyPbfHasLocationsOnWays = anyPbfHasLocationsOnWays || PbfHasOptionalFeature(file, OptionLocationsOnWays);
		}
	}

	auto createNodeStore = [allPbfsHaveSortTypeThenID, options]() {
		if (options.osm.compact) {
			std::shared_ptr<NodeStore> rv = make_shared<CompactNodeStore>();
			return rv;
		}

		if (options.inputFiles.size() == 1 && allPbfsHaveSortTypeThenID) {
			std::shared_ptr<NodeStore> rv = make_shared<SortedNodeStore>(!options.osm.uncompressedNodes);
			return rv;
		}
		std::shared_ptr<NodeStore> rv =  make_shared<BinarySearchNodeStore>();
		return rv;
	};

	shared_ptr<NodeStore> nodeStore;

	// CompactNodeStore is a dense datatype; it doesn't make sense to allocate
	// more than one of them.
	if (options.osm.shardStores && !options.osm.compact) {
		nodeStore = std::make_shared<ShardedNodeStore>(createNodeStore);
	} else {
		nodeStore = createNodeStore();
	}

	auto createWayStore = [anyPbfHasLocationsOnWays, allPbfsHaveSortTypeThenID, options, &nodeStore]() {
		if (options.inputFiles.size() == 1 && !anyPbfHasLocationsOnWays && allPbfsHaveSortTypeThenID) {
			std::shared_ptr<WayStore> rv = make_shared<SortedWayStore>(!options.osm.uncompressedWays, *nodeStore.get());
			return rv;
		}

		std::shared_ptr<WayStore> rv = make_shared<BinarySearchWayStore>();
		return rv;
	};

	shared_ptr<WayStore> wayStore;
	if (options.osm.shardStores) {
		wayStore = std::make_shared<ShardedWayStore>(createWayStore, *nodeStore.get());
	} else {
		wayStore = createWayStore();
	}

	OSMStore osmStore(*nodeStore.get(), *wayStore.get());
	osmStore.use_compact_store(options.osm.compact);
	osmStore.enforce_integrity(!options.osm.skipIntegrity);
	if(!options.osm.storeFile.empty()) {
		std::cout << "Using osm store file: " << options.osm.storeFile << std::endl;
		osmStore.open(options.osm.storeFile);
	}

	AttributeStore attributeStore;

	class LayerDefinition layers(config.layers);

	const unsigned int indexZoom = std::min(config.baseZoom, 14u);
	class OsmMemTiles osmMemTiles(options.threadNum, indexZoom, config.includeID, *nodeStore, *wayStore);
	class ShpMemTiles shpMemTiles(options.threadNum, indexZoom);
	osmMemTiles.open();
	shpMemTiles.open();

	OsmLuaProcessing osmLuaProcessing(osmStore, config, layers, options.luaFile, 
		shpMemTiles, osmMemTiles, attributeStore, options.osm.materializeGeometries, true);

	// ---- Load external sources (shp/geojson)

	{
		ShpProcessor shpProcessor(clippingBox, options.threadNum, shpMemTiles, osmLuaProcessing);
		GeoJSONProcessor geoJSONProcessor(clippingBox, options.threadNum, shpMemTiles, osmLuaProcessing);
		for (size_t layerNum=0; layerNum<layers.layers.size(); layerNum++) {
			LayerDef &layer = layers.layers[layerNum];
			if(layer.indexed) { shpMemTiles.CreateNamedLayerIndex(layer.name); }

			if (layer.source.size()>0) {
				if (!hasClippingBox) {
					cerr << "Can't read shapefiles unless a bounding box is provided." << endl;
					exit(EXIT_FAILURE);
				} else if (ends_with(layer.source, "json") || ends_with(layer.source, "jsonl") || ends_with(layer.source, "JSON") || ends_with(layer.source, "JSONL") || ends_with(layer.source, "jsonseq") || ends_with(layer.source, "JSONSEQ")) {
					cout << "Reading GeoJSON " << layer.name << endl;
					geoJSONProcessor.read(layers.layers[layerNum], layerNum);
				} else {
					cout << "Reading shapefile " << layer.name << endl;
					shpProcessor.read(layers.layers[layerNum], layerNum);
				}
			}
		}
	}
	shpMemTiles.reportSize();

	// ----	Read significant node/way tags
	const SignificantTags significantNodeTags = osmLuaProcessing.GetSignificantNodeKeys();
	const SignificantTags significantWayTags = osmLuaProcessing.GetSignificantWayKeys();

	// ----	Read all PBFs
	
	PbfProcessor pbfProcessor(osmStore);
	std::vector<bool> sortOrders = layers.getSortOrders();

	for (auto inputFile : options.inputFiles) {
		cout << "Reading .pbf " << inputFile << endl;
		ifstream infile(inputFile, ios::in | ios::binary);
		if (!infile) { cerr << "Couldn't open .pbf file " << inputFile << endl; return -1; }
		
		const bool hasSortTypeThenID = PbfHasOptionalFeature(inputFile, OptionSortTypeThenID);
		int ret = pbfProcessor.ReadPbfFile(
			nodeStore->shards(),
			hasSortTypeThenID,
			significantNodeTags,
			significantWayTags,
			options.threadNum,
			[&]() {
				thread_local std::pair<std::string, std::shared_ptr<ifstream>> pbfStream;
				if (pbfStream.first != inputFile) {
					pbfStream = std::make_pair(inputFile, std::make_shared<ifstream>(inputFile, ios::in | ios::binary));
				}
				return pbfStream.second;
			},
			[&]() {
				thread_local std::pair<std::string, std::shared_ptr<OsmLuaProcessing>> osmLuaProcessing;
				if (osmLuaProcessing.first != inputFile) {
					osmLuaProcessing = std::make_pair(inputFile, std::make_shared<OsmLuaProcessing>(osmStore, config, layers, options.luaFile, shpMemTiles, osmMemTiles, attributeStore, options.osm.materializeGeometries, false));
				}
				return osmLuaProcessing.second;
			},
			*nodeStore,
			*wayStore
		);
		if (ret != 0) return ret;
	} 
	attributeStore.finalize();
	osmMemTiles.reportSize();
	attributeStore.reportSize();
	osmLuaProcessing.dataStore.clear(); // no longer needed

	// ----	Initialise SharedData

	SourceList sources = {&osmMemTiles, &shpMemTiles};
	class SharedData sharedData(config, layers);
	sharedData.outputFile = options.outputFile;
	sharedData.outputMode = options.outputMode;
	sharedData.mergeSqlite = options.mergeSqlite;

	// ----	Initialise mbtiles/pmtiles if required
	
	if (sharedData.outputMode == OptionsParser::OutputMode::MBTiles) {
		sharedData.mbtiles.openForWriting(sharedData.outputFile);
		sharedData.writeMBTilesProjectData();
	} else if (sharedData.outputMode == OptionsParser::OutputMode::PMTiles) {
		sharedData.pmtiles.open(sharedData.outputFile);
	}

	// ----	Write out data

	// Launch the pool with threadNum threads
	boost::asio::thread_pool pool(options.threadNum);

	// Mutex is hold when IO is performed
	std::mutex io_mutex;

	// Loop through tiles
	std::atomic<uint64_t> tilesWritten(0), lastTilesWritten(0);

	for (auto source : sources) {
		source->finalize(options.threadNum);
	}
	// tiles by zoom level

	// The clipping bbox check is expensive - as an optimization, compute the set of
	// z6 tiles that are wholly covered by the clipping box. Membership in this
	// set is quick to test.
	PreciseTileCoordinatesSet coveredZ6Tiles(6);
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

	// For large areas (arbitrarily defined as 100 z6 tiles), use a dense index for pmtiles
	if (coveredZ6Tiles.size()>100 && options.outputMode == OptionsParser::OutputMode::PMTiles) {
		std::cout << "Using dense index for .pmtiles" << std::endl;
		sharedData.pmtiles.isSparse = false;
	}

	std::deque<std::pair<unsigned int, TileCoordinates>> tileCoordinates;
	std::vector<std::shared_ptr<TileCoordinatesSet>> zoomResults;
	zoomResults.reserve(sharedData.config.endZoom + 1);

	// Add PreciseTileCoordinatesSet, but only up to z14.
	for (uint zoom = 0; zoom <= std::min(14u, sharedData.config.endZoom); zoom++) {
		zoomResults.emplace_back(std::make_shared<PreciseTileCoordinatesSet>(zoom));
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

	// Add LossyTileCoordinatesSet, if needed
	for (uint zoom = 15u; zoom <= sharedData.config.endZoom; zoom++) {
		zoomResults.emplace_back(std::make_shared<LossyTileCoordinatesSet>(zoom, *zoomResults[14]));
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
				if (!zoomResult->test(x, y))
					continue;
			
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
		options.threadNum);

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

		boost::asio::post(pool, [=, &tileCoordinates, &pool, &sharedData, &sources, &attributeStore, &io_mutex, &tilesWritten, &lastTilesWritten]() {
			std::vector<std::string> tileTimings;
			std::size_t endIndex = std::min(tileCoordinates.size(), startIndex + batchSize);
			for(std::size_t i = startIndex; i < endIndex; ++i) {
				unsigned int zoom = tileCoordinates[i].first;
				TileCoordinates coords = tileCoordinates[i].second;

#ifdef CLOCK_MONOTONIC
				timespec start, end;
				if (options.logTileTimings)
					clock_gettime(CLOCK_MONOTONIC, &start);
#endif

				std::vector<std::vector<OutputObjectID>> data;
				for (auto source : sources) {
					data.emplace_back(source->getObjectsForTile(sortOrders, zoom, coords));
				}
				outputProc(sharedData, sources, attributeStore, data, coords, zoom);

#ifdef CLOCK_MONOTONIC
				if (options.logTileTimings) {
					clock_gettime(CLOCK_MONOTONIC, &end);
					uint64_t tileNs = 1e9 * (end.tv_sec - start.tv_sec) + end.tv_nsec - start.tv_nsec;
					std::string output = "z" + std::to_string(zoom) + "/" + std::to_string(coords.x) + "/" + std::to_string(coords.y) + " took " + std::to_string(tileNs/1e6) + " ms";
					tileTimings.push_back(output);
				}
#endif
			}

			if (options.logTileTimings) {
				const std::lock_guard<std::mutex> lock(io_mutex);
				std::cout << std::endl;
				for (const auto& output : tileTimings)
					std::cout << output << std::endl;
			}

			tilesWritten += (endIndex - startIndex); 

			if (io_mutex.try_lock()) {
				uint64_t written = tilesWritten.load();

				if (written >= lastTilesWritten + tileCoordinates.size() / 100 || ISATTY) {
					lastTilesWritten = written;
					// Show progress grouped by z6 (or lower)
					size_t z = tileCoordinates[startIndex].first;
					size_t x = tileCoordinates[startIndex].second.x;
					size_t y = tileCoordinates[startIndex].second.y;
					if (z > CLUSTER_ZOOM) {
						x = x / (1 << (z - CLUSTER_ZOOM));
						y = y / (1 << (z - CLUSTER_ZOOM));
						z = CLUSTER_ZOOM;
					}
					cout << "z" << z << "/" << x << "/" << y << ", writing tile " << written << " of " << tileCoordinates.size() << "               \r" << std::flush;
				}
				io_mutex.unlock();
			}
		});
	}
	// Wait for all tasks in the pool to complete.
	pool.join();

	// ----	Close tileset

	if (options.outputMode == OptionsParser::OutputMode::MBTiles) {
		sharedData.writeMBTilesMetadata(jsonConfig);
		sharedData.mbtiles.closeForWriting();
	} else if (options.outputMode == OptionsParser::OutputMode::PMTiles) {
		sharedData.writePMTilesBounds();
		std::string metadata = sharedData.pmTilesMetadata(jsonConfig);
		sharedData.pmtiles.close(metadata);
	} else {
		sharedData.writeFileMetadata(jsonConfig);
	}

#ifndef _MSC_VER
	if (verbose) {
		struct rusage r_usage;
		getrusage(RUSAGE_SELF, &r_usage);
		cout << "\nMemory used: " << r_usage.ru_maxrss << endl;
	}
#endif

	cout << endl << "Filled the tileset with good things at " << sharedData.outputFile << endl;
}

