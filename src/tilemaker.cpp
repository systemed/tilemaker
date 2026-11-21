#include <memory>
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

#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/filereadstream.h"
#include "rapidjson/filewritestream.h"
#include "rapidjson/error/en.h"
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>	// sysconf
#endif
#ifdef __linux__
#include <sys/mman.h>	// madvise
#endif

#ifndef _MSC_VER
#include <sys/resource.h>
#endif
#ifdef __GLIBC__
#include <malloc.h>
#include <cstdlib>
#include <cstring>
#endif

#include "geom.h"
#include "node_stores.h"
#include "way_stores.h"

// Tilemaker code
#include "config_validator.h"
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

void sortTileKeys(uint64_t *keys, size_t count, size_t threadNum);

// A tile of the tile list: zoom, x and y in 8 bytes rather than 12 (a planet has 346M tiles). x and
// y take 29 bits, enough up to z29.
struct ListedTile {
	static constexpr unsigned MAX_ZOOM = 29;
	uint64_t v;
	ListedTile() = default;
	ListedTile(unsigned zoom, TileCoordinates c) : v((uint64_t(zoom) << 58) | (uint64_t(c.x) << 29) | uint64_t(c.y)) { }
	unsigned zoom() const { return v >> 58; }
	TileCoordinates coords() const { return TileCoordinates((v >> 29) & ((1u << 29) - 1), v & ((1u << 29) - 1)); }
	operator std::pair<unsigned int, TileCoordinates>() const { return { zoom(), coords() }; }
};


// The order of the tile list as a 64-bit key, so that it can be sorted as plain numbers. z0..5
// first, breadth-first: (zoom, x, y). Then depth-first from z6: the z6 x and y, then at each
// further zoom the x and y of the tiles' ancestors - with equal ancestors those differ only in the
// new bit, so each zoom adds the digit (x bit, y bit). A tile whose ancestors all equal the other's
// comes first (smaller zoom): its missing digits are 0, and the zoom is the last field. Needs
// 2 * (maxZoom - 6) + 12 + 5 + 1 bits (up to z29).
static inline uint64_t tileOrderKey(unsigned zoom, TileCoordinate x, TileCoordinate y, unsigned maxZoom) {
	if (zoom < CLUSTER_ZOOM)
		return (uint64_t(zoom) << 40) | (uint64_t(x) << 20) | uint64_t(y);
	const unsigned levels = maxZoom - CLUSTER_ZOOM;	// digits after z6
	uint64_t key = (uint64_t(x >> (zoom - CLUSTER_ZOOM)) << 6) | uint64_t(y >> (zoom - CLUSTER_ZOOM));
	for (unsigned z = CLUSTER_ZOOM + 1; z <= maxZoom; z++) {
		unsigned digit = 0;
		if (z <= zoom) digit = (((x >> (zoom - z)) & 1) << 1) | ((y >> (zoom - z)) & 1);
		key = (key << 2) | digit;
	}
	(void)levels;
	return (uint64_t(1) << 63) | (key << 5) | zoom;
}

static inline ListedTile tileFromOrderKey(uint64_t key, unsigned maxZoom) {
	if (!(key >> 63)) {
		const unsigned zoom = (key >> 40) & 0xff;
		return ListedTile(zoom, TileCoordinates((key >> 20) & 0xfffff, key & 0xfffff));
	}
	const unsigned zoom = key & 31;
	uint64_t digits = (key >> 5) & ~(uint64_t(1) << 58);
	TileCoordinate x = 0, y = 0;
	for (unsigned z = maxZoom; z > CLUSTER_ZOOM; z--) {
		const unsigned digit = digits & 3;
		digits >>= 2;
		if (z <= zoom) { x |= TileCoordinate(digit >> 1) << (zoom - z); y |= TileCoordinate(digit & 1) << (zoom - z); }
	}
	x |= TileCoordinate((digits >> 6) & 63) << (zoom - CLUSTER_ZOOM);
	y |= TileCoordinate(digits & 63) << (zoom - CLUSTER_ZOOM);
	return ListedTile(zoom, TileCoordinates(x, y));
}

/**
 *\brief The Main function is responsible for command line processing, loading data and starting worker threads.
 *
 * Data is loaded into OsmMemTiles and ShpMemTiles.
 *
 * Worker threads write the output tiles, and start in the outputProc function.
 */
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <atomic>
#include <cstdio>
namespace {
// Hand the big anonymous mappings (the stores, malloc arenas) back to the kernel from several
// threads, then exit. Left to process exit, the kernel frees them on one core: ~10 s for a planet
// build. Once memory is released it reads as zeros, so nothing may be allocated or freed from
// then on: all set-up happens first, and the helper threads are never joined.
struct ReleaseJob {
	std::pair<char*, size_t> pieces[1 << 16];
	size_t count = 0;
	std::atomic<size_t> next{0};
	std::atomic<bool> go{false};
	std::atomic<unsigned> running{0};
};
ReleaseJob releaseJob;

void releasePieces(ReleaseJob &job) {
	for (size_t i; (i = job.next++) < job.count; )
		madvise(job.pieces[i].first, job.pieces[i].second, MADV_DONTNEED);
}

void *releaseWorker(void *) {
	while (!releaseJob.go.load()) sched_yield();
	releasePieces(releaseJob);
	releaseJob.running--;
	for (;;) pause();	// returning would make glibc free this thread's resources
}
}

// Early exit (the default; --no-early-exit turns it off): a process sharing tilemaker's memory
// (clone with CLONE_VM) holds on to it, so that tilemaker's own exit doesn't wait for the kernel
// to free it; the holder closes the files it
// inherited (pipes see their end now), waits until tilemaker is gone, then exits, and the kernel
// frees the memory in the background.
namespace {
pid_t earlyExitParent = 0;
int earlyExitHolder(void *) {
	for (int fd = 0; fd < (1 << 16); fd++) close(fd);
	while (getppid() == earlyExitParent) usleep(1000);
	_exit(0);
}
}

[[noreturn]] static void releaseMemoryAndExit(unsigned threadNum);

[[noreturn]] static void exitEarly(unsigned threadNum) {
	earlyExitParent = getpid();
	const size_t stackSize = 1 << 16;
	void *stack = mmap(nullptr, stackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
	if (stack == MAP_FAILED || clone(earlyExitHolder, static_cast<char *>(stack) + stackSize, CLONE_VM | SIGCHLD, nullptr) == -1)
		releaseMemoryAndExit(threadNum);
	std::_Exit(EXIT_SUCCESS);
}

[[noreturn]] static void releaseMemoryAndExit(unsigned threadNum) {
	ReleaseJob &job = releaseJob;
	const size_t pieceSize = 256ul << 20;
	const uintptr_t jobFrom = reinterpret_cast<uintptr_t>(&job), jobTo = jobFrom + sizeof(job);
	if (FILE *maps = fopen("/proc/self/maps", "r")) {
		static char line[4096];
		while (fgets(line, sizeof(line), maps)) {
			unsigned long from, to, offset, inode;
			char perms[5], dev[16];
			if (sscanf(line, "%lx-%lx %4s %lx %15s %lu", &from, &to, perms, &offset, dev, &inode) != 6) continue;
			// Private writable memory without a file, and not [heap]/[stack]. Thread stacks are
			// 8 MB, so the size limit keeps them out as well.
			if (inode != 0 || std::string(perms) != "rw-p" || strchr(line, '[') != nullptr) continue;
			if (to - from < (64ul << 20) || (from < jobTo && jobFrom < to)) continue;
			for (unsigned long p = from; p < to && job.count < (1 << 16); p += pieceSize)
				job.pieces[job.count++] = { reinterpret_cast<char*>(p), std::min<unsigned long>(pieceSize, to - p) };
		}
	}
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, 1 << 16);
	for (unsigned t = 1; t < std::min(threadNum, 32u); t++) {
		pthread_t thread;
		job.running++;
		if (pthread_create(&thread, &attr, releaseWorker, nullptr) != 0) { job.running--; break; }
	}
	job.go = true;
	releasePieces(job);
	while (job.running.load()) sched_yield();
	std::_Exit(EXIT_SUCCESS);
}
#endif

// Memory this process may use: physical RAM, or the cgroup (v2/v1) limit if lower; 0 if unknown
static uint64_t availableMemoryBytes() {
	uint64_t bytes = 0;
#if defined(__unix__) || defined(__APPLE__)
	const long pages = sysconf(_SC_PHYS_PAGES), pageSize = sysconf(_SC_PAGESIZE);
	if (pages > 0 && pageSize > 0) bytes = (uint64_t)pages * (uint64_t)pageSize;
#endif
#ifdef __linux__
	// The limits of the process's cgroup and the ones above it, from /proc/self/cgroup: "0::/path"
	// (v2), or "N:...memory...:/path" (v1). Only in a container is the process's cgroup the root
	// (a systemd service or scope is not).
	std::ifstream cgroups("/proc/self/cgroup");
	std::string line;
	while (std::getline(cgroups, line)) {
		const size_t a = line.find(':'), b = line.find(':', a + 1);
		if (a == std::string::npos || b == std::string::npos) continue;
		const std::string controllers = "," + line.substr(a + 1, b - a - 1) + ",";
		std::string dir, file;
		if (line.compare(0, a, "0") == 0 && controllers == ",,") { dir = "/sys/fs/cgroup"; file = "/memory.max"; }
		else if (controllers.find(",memory,") != std::string::npos) { dir = "/sys/fs/cgroup/memory"; file = "/memory.limit_in_bytes"; }
		else continue;
		std::string path = line.substr(b + 1);
		while (true) {
			std::ifstream f(dir + path + file);
			unsigned long long limit = 0;	// ("max": no limit)
			if (f >> limit && limit > 0 && (bytes == 0 || limit < bytes)) bytes = limit;
			if (path.empty() || path == "/") break;
			path.resize(path.find_last_of('/'));
		}
	}
#endif
	return bytes;
}

int main(const int argc, const char* argv[]) {
#ifdef __GLIBC__
	// glibc raises its mmap threshold dynamically (up to 32/64 MB) once a large
	// mmapped chunk is freed; multi-MB buffers then land in per-thread arenas
	// that are never trimmed, inflating peak RSS. Setting malloc parameters
	// explicitly turns that dynamic adjustment off, so both are set here:
	// - a 16 MB top pad makes the heap grow and shrink in large steps; with many
	//   threads allocating, the default 128 KB steps cost many brk() calls and
	//   page faults
	// - a fixed 8 MB mmap threshold: otherwise it would stay at 128 KB, and every
	//   larger buffer would be a fresh mmap with zeroed pages. Boost's validity
	//   check on a huge polygon grows such buffers over and over (one relation
	//   with 1.9M points took 3x as long).
	// Users who tune malloc via the environment keep their settings.
	if (!getenv("MALLOC_TRIM_THRESHOLD_") && !getenv("MALLOC_MMAP_THRESHOLD_") && !getenv("MALLOC_TOP_PAD_") && !getenv("GLIBC_TUNABLES")) {
		mallopt(M_TOP_PAD, 16 << 20);
		mallopt(M_MMAP_THRESHOLD, 8 << 20);
	}
#endif

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
				 maxLon=std::numeric_limits<double>::lowest(),
				 minLat=std::numeric_limits<double>::max(),
				 maxLat=std::numeric_limits<double>::lowest();
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
		FilePtr fp = openFile(options.jsonFile, "r");
		char readBuffer[65536];
		rapidjson::FileReadStream is(fp.get(), readBuffer, sizeof(readBuffer));
		jsonConfig.ParseStream(is);
		if (jsonConfig.HasParseError()) {
			// Translate the byte offset into line/column (re-read in the same text mode as openFile)
			size_t line = 1, column = 1;
			ifstream jsonFile(options.jsonFile);
			char c;
			for (size_t i = 0; i < jsonConfig.GetErrorOffset() && jsonFile.get(c); i++) {
				if (c == '\n') { line++; column = 1; } else column++;
			}
			cerr << "Invalid JSON file: " << rapidjson::GetParseError_En(jsonConfig.GetParseError())
			     << " at line " << line << ", column " << column << "." << endl;
			return -1;
		}

		string jsonError;
		if (!validateConfigJson(jsonConfig, jsonError)) {
			cerr << "Invalid JSON file: " << jsonError << "." << endl;
			return -1;
		}

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

	class Declutter declutter;
	declutter.configure(layers.layers);

	const unsigned int indexZoom = std::min(config.baseZoom, 14u);
	class OsmMemTiles osmMemTiles(options.threadNum, indexZoom, config.includeID, *nodeStore, *wayStore);
	class ShpMemTiles shpMemTiles(options.threadNum, indexZoom, declutter);
	osmMemTiles.open();
	shpMemTiles.open();

	OsmLuaProcessing osmLuaProcessing(osmStore, config, layers, options.luaFile, 
		shpMemTiles, osmMemTiles, attributeStore, declutter, options.osm.materializeGeometries, true);

	// ----	Read significant node/way tags (before the external sources: they use this Lua state)
	const SignificantTags significantNodeTags = osmLuaProcessing.GetSignificantNodeKeys();
	const SignificantTags significantWayTags = osmLuaProcessing.GetSignificantWayKeys();

	// ---- Load external sources (shp/geojson)

	bool anyIndexed = false, anyExternal = false;
	for (auto const &layer : layers.layers) {
		if (layer.indexed) anyIndexed = true;
		if (layer.source.size() > 0) anyExternal = true;
	}
	if (anyExternal && !hasClippingBox) {
		cerr << "Can't read shapefiles unless a bounding box is provided." << endl;
		exit(EXIT_FAILURE);
	}
	int externalResult = 0;
	auto readExternal = [&]() {
		ShpProcessor shpProcessor(clippingBox, options.threadNum, shpMemTiles, osmLuaProcessing);
		GeoJSONProcessor geoJSONProcessor(clippingBox, options.threadNum, shpMemTiles, osmLuaProcessing);
		for (size_t layerNum=0; layerNum<layers.layers.size(); layerNum++) {
			LayerDef &layer = layers.layers[layerNum];
			if(layer.indexed) { shpMemTiles.CreateNamedLayerIndex(layer.name); }

			if (layer.source.size()>0) {
				try {
					if (ends_with(layer.source, "json") || ends_with(layer.source, "jsonl") || ends_with(layer.source, "JSON") || ends_with(layer.source, "JSONL") || ends_with(layer.source, "jsonseq") || ends_with(layer.source, "JSONSEQ")) {
						cout << "Reading GeoJSON " << layer.name << endl;
						geoJSONProcessor.read(layers.layers[layerNum], layerNum);
					} else {
						cout << "Reading shapefile " << layer.name << endl;
						shpProcessor.read(layers.layers[layerNum], layerNum);
					}
				} catch (const std::exception& e) {
					cerr << "Error reading external source " << layer.source << ": " << e.what() << endl;
					externalResult = -1;
					return;
				} catch (...) {
					cerr << "Unknown error reading external source " << layer.source << endl;
					externalResult = -1;
					return;
				}
			}
		}
	};
	// Unless a layer is indexed (the .pbf's Lua code may look things up in it), nothing read from
	// the .pbf depends on the external sources, so read them alongside the .pbf: the ocean shapefile
	// alone took ~11 s, mostly on a few threads, at the start of a planet run.
	std::thread externalThread;
	struct JoinOnExit { std::thread &t; ~JoinOnExit() { if (t.joinable()) t.join(); } } joinExternal { externalThread };
	if (anyIndexed || !anyExternal) {
		readExternal();
		if (externalResult != 0) return externalResult;
		shpMemTiles.reportSize();
	} else {
		externalThread = std::thread(readExternal);
	}

	// ----	Read all PBFs
	
	PbfProcessor pbfProcessor(osmStore);
	std::vector<bool> sortOrders = layers.getSortOrders();

	for (auto inputFile : options.inputFiles) {
		cout << "Reading .pbf " << inputFile << endl;
		ifstream infile(inputFile, ios::in | ios::binary);
		if (!infile) { cerr << "Couldn't open .pbf file " << inputFile << endl; return -1; }
		
		const bool hasSortTypeThenID = PbfHasOptionalFeature(inputFile, OptionSortTypeThenID);
		int ret;
		try {
			ret = pbfProcessor.ReadPbfFile(
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
						osmLuaProcessing = std::make_pair(inputFile, std::make_shared<OsmLuaProcessing>(osmStore, config, layers, options.luaFile, shpMemTiles, osmMemTiles, attributeStore, declutter, options.osm.materializeGeometries, false));
					}
					return osmLuaProcessing.second;
				},
				*nodeStore,
				*wayStore
			);
		} catch (const std::exception& e) {
			cerr << endl << "Error reading " << inputFile << ": " << e.what() << endl;
			return -1;
		}
		if (ret != 0) return ret;
	} 
	if (externalThread.joinable()) {
		externalThread.join();
		if (externalResult != 0) return externalResult;
		shpMemTiles.reportSize();
	}
	attributeStore.finalize();
	osmMemTiles.reportSize();
	attributeStore.reportSize();
	osmLuaProcessing.dataStore.clear(); // no longer needed
	osmStore.releaseReadingState();
#ifdef __GLIBC__
	// glibc keeps what the reading phase freed (Lua memo trees, way buffers: ~0.5 GB on Netherlands
	// at 8 threads; the relation scan, allocated first, ~1.1 GB on a planet) in its arenas: give the
	// free pages back before the tiles are made
	malloc_trim(0);
#endif

	// ----	Initialise SharedData

	SourceList sources = {&osmMemTiles, &shpMemTiles};
	class SharedData sharedData(config, layers);
	sharedData.outputFile = options.outputFile;
	sharedData.outputMode = options.outputMode;
	sharedData.mergeSqlite = options.mergeSqlite;
	if (sharedData.mergeSqlite && config.tileFormat == TileFormat::MLT) {
		cerr << "--merge is not supported with \"tile_format\": \"mlt\"" << endl;
		return 0;
	}

	// ----	Initialise mbtiles/pmtiles if required
	
	if (sharedData.outputMode == OptionsParser::OutputMode::MBTiles) {
		sharedData.mbtiles.openForWriting(sharedData.outputFile);
		sharedData.writeMBTilesProjectData();
	} else if (sharedData.outputMode == OptionsParser::OutputMode::PMTiles) {
		sharedData.pmtiles.open(sharedData.outputFile);
		sharedData.pmtiles.compressionLevel = config.compressionLevel;
		if (config.tileFormat == TileFormat::MLT) sharedData.pmtiles.tileType = TILETYPE_MLT;
	}

	// ----	Write out data

	// Launch the pool with threadNum threads
	boost::asio::thread_pool pool(options.threadNum);

	// Mutex is hold when IO is performed
	std::mutex io_mutex;

	// Loop through tiles
	std::atomic<uint64_t> tilesWritten(0), lastTilesWritten(0);

	// Rank any decluttered point features, and index them at their allotted zoom levels
	declutter.apply(layers.layers, osmMemTiles, shpMemTiles);

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

	std::vector<ListedTile> tileCoordinates;
	// z(SHARED_PIECE_ZOOM) tiles below which a tile is written (for the shared pieces)
	const unsigned pieceZoom = TileDataSource::SHARED_PIECE_ZOOM;
	std::vector<uint8_t> piecePresent((size_t)1 << (2 * pieceZoom), 0);
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
		if (options.threadNum > 1 && sharedData.config.endZoom >= 12) z12ObjectCounts.assign((size_t)1 << 24, 0);
		populateTilesAtZoom(sources, zoomResults, options.threadNum);
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
	// Parts of the tile list, in order (zoom by zoom, stripe by stripe): copied into one allocation
	// at the end, on all threads
	std::vector<std::vector<ListedTile>> listParts;
	for (uint zoom=sharedData.config.startZoom; zoom <= sharedData.config.endZoom; zoom++) {
		std::cout << " z" << std::to_string(zoom) << std::flush;
#ifdef CLOCK_MONOTONIC
		timespec start, end;
		clock_gettime(CLOCK_MONOTONIC, &start);
#endif

		const auto& zoomResult = zoomResults[zoom];
		auto keep = [&](TileCoordinate x, TileCoordinate y) {
			if (hasClippingBox) {
				bool isInAWhollyCoveredZ6Tile = false;
				if (zoom >= 6) {
					TileCoordinate z6x = x / (1 << (zoom - 6));
					TileCoordinate z6y = y / (1 << (zoom - 6));
					isInAWhollyCoveredZ6Tile = coveredZ6Tiles.test(z6x, z6y);
				}

				if(!isInAWhollyCoveredZ6Tile && !boost::geometry::intersects(TileBbox(TileCoordinates(x, y), zoom, false, false).getTileBox(), clippingBox)) 
					return false;
			}
			return true;
		};
		auto markPresent = [&](std::vector<uint8_t> &present, TileCoordinate x, TileCoordinate y) {
			if (zoom > pieceZoom)
				present[((size_t)(x >> (zoom - pieceZoom)) << pieceZoom) + (y >> (zoom - pieceZoom))] = 1;
		};
		size_t numTiles = 0;
		const auto *precise = dynamic_cast<const PreciseTileCoordinatesSet*>(zoomResult.get());
		const TileCoordinate dim = 1u << std::min(zoom, 30u);
		if (precise && zoom >= 6 && options.threadNum > 1) {
			// Column stripes in parallel (z14 alone has ~260M tiles on the planet); the order
			// doesn't matter, the list is sorted below.
			const size_t stripes = std::min<size_t>(dim, options.threadNum * 8);
			std::vector<std::vector<ListedTile>> parts(stripes);
			std::vector<std::vector<uint8_t>> presents(options.threadNum);
			std::atomic<size_t> nextStripe(0);
			boost::asio::thread_pool pool(options.threadNum);
			for (unsigned t = 0; t < options.threadNum; t++) {
				boost::asio::post(pool, [&, t]() {
					std::vector<uint8_t> &present = presents[t];
					present.assign(piecePresent.size(), 0);
					for (size_t s = nextStripe++; s < stripes; s = nextStripe++) {
						const TileCoordinate x0 = (uint64_t)dim * s / stripes, x1 = (uint64_t)dim * (s + 1) / stripes;
						precise->forEachSetInColumns(x0, x1, [&](TileCoordinate x, TileCoordinate y) {
							if (!keep(x, y)) return;
							parts[s].emplace_back(zoom, TileCoordinates(x, y));
							markPresent(present, x, y);
						});
					}
				});
			}
			pool.join();
			for (auto &part : parts) {
				numTiles += part.size();
				listParts.emplace_back(std::move(part));
			}
			for (auto const &present : presents)
				for (size_t i = 0; i < present.size(); i++) piecePresent[i] |= present[i];
		} else {
			listParts.emplace_back();
			auto &part = listParts.back();
			zoomResult->forEachSet([&](TileCoordinate x, TileCoordinate y) {
				if (!keep(x, y)) return;
				part.emplace_back(zoom, TileCoordinates(x, y));
				markPresent(piecePresent, x, y);
				numTiles++;
			});
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
	const unsigned listMaxZoom = std::max<unsigned>(CLUSTER_ZOOM, sharedData.config.endZoom);
	{
		std::vector<size_t> offsets(listParts.size() + 1, 0);
		for (size_t i = 0; i < listParts.size(); i++) offsets[i + 1] = offsets[i] + listParts[i].size();
		const size_t total = offsets.back();
		std::unique_ptr<uint64_t[]> keys(new uint64_t[std::max<size_t>(1, total)]);	// not zeroed: every slot is written
		{
			std::atomic<size_t> nextPart(0);
			boost::asio::thread_pool keyPool(options.threadNum);
			for (unsigned t = 0; t < options.threadNum; t++) {
				boost::asio::post(keyPool, [&]() {
					for (size_t i = nextPart++; i < listParts.size(); i = nextPart++) {
						uint64_t *out = keys.get() + offsets[i];
						for (const ListedTile &tile : listParts[i]) { const TileCoordinates c = tile.coords(); *out++ = tileOrderKey(tile.zoom(), c.x, c.y, listMaxZoom); }
						std::vector<ListedTile>().swap(listParts[i]);
					}
				});
			}
			keyPool.join();
		}
		listParts.clear();
		sortTileKeys(keys.get(), total, options.threadNum);
		// The sorted list is made only now: alongside the parts, the keys and the sort's own buffer of
		// as many keys, it raised the peak by its size (2.8 GB for a planet's 346M tiles). resize() writes
		// every element from one thread, which on fresh memory is mostly page faults (1.8 s for 346M
		// tiles): touch it on all threads first.
		tileCoordinates.reserve(total);
		{
			char *storage = reinterpret_cast<char *>(tileCoordinates.data());
			const size_t bytes = total * sizeof(tileCoordinates[0]), chunk = 64 << 20;
			std::atomic<size_t> nextChunk(0);
			boost::asio::thread_pool touchPool(options.threadNum);
			for (unsigned t = 0; t < options.threadNum; t++) {
				boost::asio::post(touchPool, [&]() {
					for (size_t c = nextChunk++; c * chunk < bytes; c = nextChunk++)
						std::memset(storage + c * chunk, 0, std::min(chunk, bytes - c * chunk));
				});
			}
			touchPool.join();
		}
		tileCoordinates.resize(total);
		{
			const size_t chunk = 1 << 20;
			std::atomic<size_t> nextChunk(0);
			boost::asio::thread_pool decodePool(options.threadNum);
			for (unsigned t = 0; t < options.threadNum; t++) {
				boost::asio::post(decodePool, [&]() {
					for (size_t c = nextChunk++; c * chunk < total; c = nextChunk++)
						for (size_t i = c * chunk; i < std::min(total, (c + 1) * chunk); i++)
							tileCoordinates[i] = tileFromOrderKey(keys[i], listMaxZoom);
				});
			}
			decodePool.join();
		}
	}

	std::cout << std::endl;

	OsmLuaProcessing::printMemoStats();

	// Validity of big relations, checked while the rest was read (see checkValidityLater)
	for (auto source : sources) source->finishDeferredValidity();

	// Shared pieces of large polygons, for the SHARED_PIECE_ZOOM tiles below which a tile is written
	{
		auto present = [&](TileCoordinate x, TileCoordinate y) {
			return x < (1u << pieceZoom) && y < (1u << pieceZoom) && piecePresent[((size_t)x << pieceZoom) + y];
		};
		for (auto source : sources) source->precomputeSharedPieces(present, options.threadNum);
	}

	std::atomic<uint64_t> batchCounter(0);
	// First error from a tile-writing worker; reported after pool.join()
	// (an exception escaping a pool thread would call std::terminate)
	std::atomic<bool> tileFailed(false);
	std::string tileError;
	std::mutex tileErrorMutex;
	auto recordTileError = [&](unsigned int zoom, TileCoordinates coords, const std::string& what) {
		const std::lock_guard<std::mutex> lock(tileErrorMutex);
		if (tileFailed) return;
		tileError = "z" + std::to_string(zoom) + "/" + std::to_string(coords.x) + "/" + std::to_string(coords.y) + ": " + what;
		tileFailed = true;
	};
	// A write of the ordered writers failed (see OrderedBatches::guarded): stop as for a tile
	auto writeFailed = [&]() { return sharedData.pmtiles.batches.failed() || sharedData.mbtiles.batches.failed(); };
	const uint cutZoom = std::max<uint>(12, config.startZoom);
	// Batches are handed out as threads become free rather than all posted up
	// front: a planet has ~14M of them, and their queued handlers held GBs.
	std::mutex batchMutex;
	uint64_t nextBatchSeq = 0;
	// Shared pieces (TileDataSource::precomputeSharedPieces) lie inside one z6 tile. Tiles from z6 on
	// are written z6 tile by z6 tile, so once the hand-out has moved past a z6 tile and none of its
	// batches is still running, no tile needs its pieces any more.
	std::vector<uint32_t> z6Running(CLUSTER_ZOOM_WIDTH * CLUSTER_ZOOM_WIDTH, 0);
	std::atomic<int64_t> z6HandedOut(-1);	// changed under batchMutex; read without it for the pieces
	// The pieces are computed ahead of the hand-out while those alive take less than this: all
	// computed at the start, the planet's took about 5 GB, more than the tile phase's other memory.
	const uint64_t pieceBudget = std::max<uint64_t>(256ull << 20, availableMemoryBytes() / 32);
	auto z6Of = [&](std::size_t i) -> int64_t {
		const unsigned z = tileCoordinates[i].zoom();
		if (z < CLUSTER_ZOOM) return -1;
		const TileCoordinates c = tileCoordinates[i].coords();
		return (int64_t)(c.x >> (z - CLUSTER_ZOOM)) * CLUSTER_ZOOM_WIDTH + (c.y >> (z - CLUSTER_ZOOM));
	};
	// The pieces are freed by the threads that computed them, mostly into the middle of their malloc
	// arenas, where glibc keeps the pages (the planet's pieces took several GB at the start of the
	// tile phase): once enough is freed, give it back, outside batchMutex
	std::atomic<uint64_t> releasedPieceBytes(0);
	auto releaseZ6 = [&](int64_t b) {
		for (auto source : sources) releasedPieceBytes += source->releaseSharedPieces(b / CLUSTER_ZOOM_WIDTH, b % CLUSTER_ZOOM_WIDTH);
	};
	auto trimReleased = [&]() {
#ifdef __GLIBC__
		if (releasedPieceBytes.load(std::memory_order_relaxed) >= (256u << 20) && releasedPieceBytes.exchange(0) >= (256u << 20))
			malloc_trim(0);
#endif
	};
	// Batches are a fixed partition: each z<=11 tile is its own batch, and
	// z12 subtrees are grouped (~2 per batch), so z13/z14 tiles usually share
	// a batch with their z12 ancestor (batch-local ClipCache can reuse its clips).
	// The partition doesn't depend on thread count, so neither does the output.
	std::vector<std::size_t> batchStarts;	// batch i: tiles batchStarts[i] .. batchStarts[i+1]
	// Appends the starts of the batches from `start` (a batch start) on, up to `stop`. Returns
	// whether `stop` is a batch start.
	auto scanBatches = [&](std::size_t start, std::size_t stop, std::vector<std::size_t>& starts) {
		while (start < stop) {
			starts.push_back(start);
			// Higher-zoom tiles are cheaper to compute, lower-zoom tiles more expensive
			std::size_t size = 0, weight = 0;
			while (start + size < tileCoordinates.size()) {
				const unsigned zoom = tileCoordinates[start + size].zoom();
				if (size > 0 && (weight >= 1000 || (weight >= 50 && zoom <= cutZoom)))
					break;
				if (zoom > 12)
					weight++;
				else if (zoom > 11)
					weight += 10;
				else if (zoom > 10)
					weight += 100;
				else
					weight += 1000;
				size++;
			}
			start += size;
		}
		return start == stop;
	};
	{
		// One pass over the planet's 346M tiles took 0.5 s on one thread. The list is cut into parts
		// at tiles expected to start a batch: after a tile of z11 or below (weight >= 100), the next
		// tile of z12 or below is one (unless a run of 1000 z13/z14 tiles came between). Each part is
		// scanned from its start on its own; if a part doesn't end exactly at the next one's start,
		// the expectation was wrong and the whole list is scanned again, from the start.
		const std::size_t n = tileCoordinates.size();
		std::vector<std::size_t> cuts { 0 };
		const std::size_t numParts = options.threadNum > 1 ? (std::size_t)options.threadNum * 4 : 1;
		for (std::size_t k = 1; k < numParts; k++) {
			std::size_t i = std::max(cuts.back() + 1, n * k / numParts);
			while (i < n && tileCoordinates[i].zoom() > 11) i++;
			i++;
			while (i < n && tileCoordinates[i].zoom() > cutZoom) i++;
			if (i < n) cuts.push_back(i);
		}
		cuts.push_back(n);
		std::vector<std::vector<std::size_t>> partStarts(cuts.size() - 1);
		std::atomic<bool> allExact(true);
		std::atomic<std::size_t> nextPart(0);
		{
			boost::asio::thread_pool partPool(options.threadNum);
			for (unsigned t = 0; t < options.threadNum; t++) {
				boost::asio::post(partPool, [&]() {
					for (std::size_t k; (k = nextPart++) < partStarts.size(); )
						if (!scanBatches(cuts[k], cuts[k + 1], partStarts[k])) allExact = false;
				});
			}
			partPool.join();
		}
		if (allExact) {
			std::size_t total = 0;
			for (const auto& part : partStarts) total += part.size();
			batchStarts.reserve(total + 1);
			for (auto& part : partStarts) {
				batchStarts.insert(batchStarts.end(), part.begin(), part.end());
				std::vector<std::size_t>().swap(part);
			}
		} else {
			scanBatches(0, n, batchStarts);
		}
	}
	batchStarts.push_back(tileCoordinates.size());
	// Handed out first: the tiles up to this zoom. One of them can take half a minute (a fjord
	// coast at z6/z7); in its place in the order, the batches after it piled up until all
	// threads waited for it. They are written in their place all the same, and are small.
	const unsigned prefetchZoom = 8;
	// Consecutive batches are handed out and written in groups: on a planet there are ~14M
	// batches, and taking and handing over each one (two shared locks) limited the tile phase.
	// Each batch of a group is still worked on as its own batch, so the output is the same.
	// Prefetched batches stay in groups of their own. groupStarts[g]: first batch of group g.
	// With few batches per thread (an extract) they're handed out one by one, as grouping
	// would only make the load less even; a planet has groups of 8 (4..8 measured best).
	const std::size_t batchesPerThread = (batchStarts.size() - 1) / std::max(1u, options.threadNum);
	std::size_t groupSize = std::min<std::size_t>(8, std::max<std::size_t>(1, batchesPerThread / 16384));
	std::vector<std::size_t> groupStarts;
	for (std::size_t b = 0; b + 1 < batchStarts.size(); ) {
		groupStarts.push_back(b);
		const bool prefetch = tileCoordinates[batchStarts[b]].zoom() <= prefetchZoom;
		std::size_t n = 1;
		while (!prefetch && n < groupSize && b + n + 1 < batchStarts.size() && tileCoordinates[batchStarts[b + n]].zoom() > prefetchZoom) n++;
		b += n;
	}
	groupStarts.push_back(batchStarts.size() - 1);
	std::cout << "Tile batches: " << (batchStarts.size() - 1) << " in " << (groupStarts.size() - 1) << " groups of up to " << groupSize << std::endl;
	std::vector<uint64_t> handOutOrder;	// group numbers
	handOutOrder.reserve(groupStarts.size() - 1);
	for (uint64_t g = 0; g + 1 < groupStarts.size(); g++)
		if (tileCoordinates[batchStarts[groupStarts[g]]].zoom() <= prefetchZoom) handOutOrder.push_back(g);
	const std::size_t prefetched = handOutOrder.size();
	for (uint64_t g = 0; g + 1 < groupStarts.size(); g++)
		if (tileCoordinates[batchStarts[groupStarts[g]]].zoom() > prefetchZoom) handOutOrder.push_back(g);
	std::size_t nextHandOut = 0;

	// The groups expected to be slowest (a big city's z12 tiles took up to 7 s, while all the
	// batches behind it waited to be written; at the end of the planet the tiles only came out 4 s
	// after the rest was done) are handed out ahead of their place, when the hand-out comes within
	// slowWindow groups of them. Expected cost: the objects in the group's z12 tiles (a z13 tile a
	// quarter of its z12 tile's, a z11 tile its four z12 tiles', ...).
	std::vector<std::size_t> slowPos;	// positions in handOutOrder, ascending
	std::vector<uint8_t> handedEarly(handOutOrder.size(), 0);
	std::size_t nextSlow = 0, slowWindow = 0;
	if (!z12ObjectCounts.empty() && handOutOrder.size() > prefetched) {
		const double slowFraction = 0.001, windowFraction = 0.05;
		const std::size_t n = handOutOrder.size() - prefetched;
		std::vector<double> cost(n, 0);
		{
			std::atomic<std::size_t> nextChunk(0);
			const std::size_t chunk = 4096;
			boost::asio::thread_pool costPool(options.threadNum);
			for (unsigned t = 0; t < options.threadNum; t++) {
				boost::asio::post(costPool, [&]() {
					for (std::size_t c = nextChunk++; c * chunk < n; c = nextChunk++) {
						for (std::size_t k = c * chunk; k < std::min(n, (c + 1) * chunk); k++) {
							const uint64_t g = handOutOrder[prefetched + k];
							double sum = 0;
							for (std::size_t i = batchStarts[groupStarts[g]]; i < batchStarts[groupStarts[g + 1]]; i++) {
								const unsigned z = tileCoordinates[i].zoom();
								const TileCoordinates tc = tileCoordinates[i].coords();
								if (z >= 12) {
									sum += z12ObjectCounts[((size_t)(tc.x >> (z - 12)) << 12) | (tc.y >> (z - 12))] / double(1ull << (2 * (z - 12)));
								} else {
									const unsigned d = 12 - z;
									for (TileCoordinate x = tc.x << d; x < (tc.x + 1) << d; x++)
										for (TileCoordinate y = tc.y << d; y < (tc.y + 1) << d; y++)
											sum += z12ObjectCounts[((size_t)x << 12) | y];
								}
							}
							cost[k] = sum;
						}
					}
				});
			}
			costPool.join();
		}
		const std::size_t slowCount = std::min(n, (std::size_t)(n * slowFraction));
		if (slowCount > 0) {
			std::vector<double> sorted(cost);
			std::nth_element(sorted.begin(), sorted.begin() + (n - slowCount), sorted.end());
			const double threshold = sorted[n - slowCount];
			for (std::size_t k = 0; k < n && slowPos.size() < slowCount; k++)
				if (cost[k] >= threshold && cost[k] > 0) slowPos.push_back(prefetched + k);
			slowWindow = std::max<std::size_t>(1, (std::size_t)(n * windowFraction));
			std::cout << "Groups handed out early: " << slowPos.size() << " (expected cost >= " << threshold << "), up to " << slowWindow << " groups ahead" << std::endl;
		}
		std::vector<uint32_t>().swap(z12ObjectCounts);
	}

	// Work stealing at the end. A group's tiles are worked on in units: a batch is cut before each tile
	// of z12 or below (cutZoom), so a unit is a tile below z12, or a z12 tile with its descendants.
	// The thread the group was handed out to takes its units from the front; once all groups are
	// handed out, threads that ran out of work take units from the back of the groups still being
	// worked on (at the end of a planet run, one group of 172 tiles ran on alone for 2 s).
	// - A unit's tiles reuse clips only of their ancestors in the same unit (ClipCache), and the
	//   covered-z12 reuse starts at its z12 tile, so a unit comes out the same on any thread and
	//   after any other unit; a thread starts a new clip-cache batch at a batch start, and wherever
	//   it doesn't go on with the unit right after its last one.
	// - The writers number the units (batch << 16 | unit within the batch) and write them in that
	//   order, a run of units worked on one after another as one, so the file stays the same.
	struct GroupWork {
		std::mutex m;
		bool active = false;
		uint64_t group = 0;
		std::vector<std::size_t> unitStart;	// tile index; units are followed by the next one's start
		std::vector<uint64_t> unitKey;	// writer number; unitKey.back() is the number after the group
		std::vector<uint8_t> unitNoStore;	// in a batch of a single tile
		std::size_t front = 0, back = 0;	// units not taken yet: front .. back-1
	};
	std::vector<std::unique_ptr<GroupWork>> groupWork;
	for (unsigned t = 0; t < options.threadNum; t++) groupWork.emplace_back(new GroupWork());
	std::atomic<unsigned> nextWorker(0);
	std::vector<uint8_t> groupCountsZ6(groupStarts.size() - 1, 0);
	static thread_local std::vector<int64_t> batchZ6;	// z6 tiles of the batch this thread is working on
	// Groups handed out ahead of their place (prefetched, slow) are held until their turn. When that
	// is more than the writer may hold, the threads must not all wait (waitForRoom) while the group
	// to be written next isn't handed out: the writer is told the first group in file order not
	// handed out yet, and lets one thread go on to take it.
	const std::size_t numGroups = groupStarts.size() - 1;
	std::vector<uint8_t> groupTaken(numGroups, 0);
	std::vector<std::size_t> handOutPos(numGroups);	// position in handOutOrder
	for (std::size_t k = 0; k < handOutOrder.size(); k++) handOutPos[handOutOrder[k]] = k;
	std::size_t firstUntaken = 0;
	// its first unit's writer number (see GroupWork)
	std::atomic<uint64_t> firstUntakenKey(numGroups > 0 ? (uint64_t)groupStarts[0] << 16 : UINT64_MAX);
	// Hands out the next group: returns 0 if none is left, 1 if one is. With `needed` (a writer
	// number from waitForRoom), only the group starting with it, the first in file order not handed
	// out yet: 2 if it was handed out meanwhile.
	auto nextBatch = [&](std::size_t &startIndex, std::size_t &batchSize, uint64_t &seq, uint64_t needed) -> int {
		const std::lock_guard<std::mutex> lock(batchMutex);
		batchZ6.clear();
		while (nextHandOut < handOutOrder.size() && handedEarly[nextHandOut]) nextHandOut++;
		while (nextSlow < slowPos.size() && (slowPos[nextSlow] < nextHandOut || handedEarly[slowPos[nextSlow]])) nextSlow++;
		if (nextHandOut >= handOutOrder.size()) return 0;
		// A slow group coming up soon goes first; it doesn't move the hand-out on (z6HandedOut)
		bool early = false;
		std::size_t k;
		if (needed != UINT64_MAX) {
			if (firstUntaken >= numGroups || ((uint64_t)groupStarts[firstUntaken] << 16) != needed) return 2;
			k = handOutPos[firstUntaken];
			if (k == nextHandOut) {
				nextHandOut++;
			} else {
				handedEarly[k] = 1;
				early = true;
			}
		} else if (nextSlow < slowPos.size() && slowPos[nextSlow] > nextHandOut && slowPos[nextSlow] <= nextHandOut + slowWindow) {
			k = slowPos[nextSlow++];
			handedEarly[k] = 1;
			early = true;
		} else {
			k = nextHandOut++;
		}
		seq = handOutOrder[k];
		startIndex = batchStarts[groupStarts[seq]];
		batchSize = batchStarts[groupStarts[seq + 1]] - startIndex;
		groupTaken[seq] = 1;
		while (firstUntaken < numGroups && groupTaken[firstUntaken]) firstUntaken++;
		firstUntakenKey = firstUntaken < numGroups ? (uint64_t)groupStarts[firstUntaken] << 16 : UINT64_MAX;
		// Prefetched tiles don't use the shared pieces, and are out of order: only the others count
		// for releasing the pieces of z6 tiles the hand-out has moved past
		if (k < prefetched) return 1;
		groupCountsZ6[seq] = 1;
		for (std::size_t i = startIndex; i < startIndex + batchSize; i++) {
			const int64_t b = z6Of(i);
			if (b >= 0 && (batchZ6.empty() || batchZ6.back() != b)) batchZ6.push_back(b);
		}
		for (int64_t b : batchZ6) z6Running[b]++;
		if (!early && !batchZ6.empty() && batchZ6.back() != z6HandedOut) {
			if (z6HandedOut >= 0 && z6Running[z6HandedOut] == 0) releaseZ6(z6HandedOut);
			z6HandedOut = batchZ6.back();
		}
		return 1;
	};

	// .pmtiles and .mbtiles are written in batch order, so the file doesn't depend on thread scheduling
	const bool toPMTiles = options.outputMode == OptionsParser::OutputMode::PMTiles;
	const bool toMBTiles = options.outputMode == OptionsParser::OutputMode::MBTiles;
	// While an earlier batch is still being worked on, threads may run ahead by 1/16 of the memory
	// (physical RAM, or the cgroup limit if lower), at least 64 MB of finished tiles per thread and at
	// most 4 GB. With less, threads often wait for a slow batch (a dense z12 area, or a big
	// multipolygon read from --store files), above all when two threads share a core and each runs at
	// half speed: a 16-thread planet with --store took 54 min with 1 GB, 25 min with 4 GB.
	const uint64_t maxHeldBytes = std::max<uint64_t>((64ull << 20) * options.threadNum,
		std::min<uint64_t>(4ull << 30, availableMemoryBytes() / 16));
	if (toPMTiles) sharedData.pmtiles.batches.setMaxHeldBytes(maxHeldBytes);
	if (toMBTiles) sharedData.mbtiles.batches.setMaxHeldBytes(maxHeldBytes);
	if (toPMTiles) sharedData.pmtiles.batches.setFirstUnhanded(&firstUntakenKey);
	if (toMBTiles) sharedData.mbtiles.batches.setFirstUnhanded(&firstUntakenKey);

	// Below a z12 tile whose objects, down to the last zoom, are all polygons covering the whole
	// tile (open sea, large forests or ice), every tile of a zoom comes out the same: one per zoom
	// is computed and its bytes are saved for the others. A z12 subtree is in the same batch as its
	// z12 tile (see nextBatch).
	const unsigned coverZoom = 12;
	const bool reuseCovered = toPMTiles && !sharedData.mergeSqlite && !sharedData.config.clippingBoxFromJSON && config.endZoom > coverZoom;
	auto coveredByPolygons = [&](TileCoordinates coords) {
		objectsUpToZoom = config.endZoom;
		{
			// Most tiles have some object that isn't a polygon, or none: find out without sorting them
			bool any = false, other = false;
			for (auto source : sources) {
				const int kinds = source->objectKindsForTile(coverZoom, coords);
				if (kinds == 2) { other = true; break; }
				if (kinds == 1) any = true;
			}
			if (other || !any) { objectsUpToZoom = 0; return false; }
		}
		std::vector<std::vector<OutputObjectID>> objects;
		bool any = false, polygons = true;
		for (auto source : sources) {
			objects.emplace_back(source->getObjectsForTile(sortOrders, attributeStore, coverZoom, coords));
			for (const auto &o : objects.back()) {
				any = true;
				if (o.oo.geomType != POLYGON_) polygons = false;
			}
		}
		objectsUpToZoom = 0;
		if (!any || !polygons) return false;
		// Clipped to the tile, each must be exactly the clipping box. These clips aren't kept in
		// the clip cache, so the tiles below are built from the same clips as without this check.
		const TileBbox bbox(coords, coverZoom, false, false);
		const Box &clip = bbox.clippingBox;
		const bool noStore = clipCacheNoStore;
		clipCacheNoStore = true;
		bool covered = true;
		for (std::size_t k = 0; k < sources.size() && covered; k++) {
			for (const auto &o : objects[k]) {
				const Geometry g = sources[k]->buildWayGeometry(o.oo.geomType, o.oo.objectID, bbox);
				const MultiPolygon *mp = boost::get<MultiPolygon>(&g);
				if (!mp || mp->size() != 1 || !(*mp)[0].inners().empty() || (*mp)[0].outer().size() != 5) { covered = false; break; }
				Box env;
				geom::envelope(*mp, env);
				if (env.min_corner().x() != clip.min_corner().x() || env.min_corner().y() != clip.min_corner().y() ||
				    env.max_corner().x() != clip.max_corner().x() || env.max_corner().y() != clip.max_corner().y()) { covered = false; break; }
			}
		}
		clipCacheNoStore = noStore;
		return covered;
	};

	// The tile list (8 bytes a tile: 2.8 GB for a planet) is only read up to the tiles not written yet.
	// The pages of the written part are given back as the tiles go, rather than all at the end.
	std::mutex releaseMutex;
	std::size_t tilesReleased = 0;	// tiles before this one are given back
	auto releaseWrittenTiles = [&]() {
#ifdef __linux__
		if (!toPMTiles && !toMBTiles) return;
		const uint64_t nextKey = toPMTiles ? sharedData.pmtiles.batches.nextToWrite() : sharedData.mbtiles.batches.nextToWrite();
		const std::size_t written = batchStarts[std::min<uint64_t>(nextKey >> 16, batchStarts.size() - 1)];
		const std::size_t step = (64u << 20) / sizeof(tileCoordinates[0]);
		if (!releaseMutex.try_lock()) return;
		if (written < tilesReleased + step) { releaseMutex.unlock(); return; }
		const std::size_t page = sysconf(_SC_PAGESIZE);
		const uintptr_t base = reinterpret_cast<uintptr_t>(tileCoordinates.data());
		const uintptr_t from = (base + tilesReleased * sizeof(tileCoordinates[0]) + page - 1) / page * page;
		const uintptr_t to = (base + written * sizeof(tileCoordinates[0])) / page * page;
		if (to > from) madvise(reinterpret_cast<void *>(from), to - from, MADV_DONTNEED);
		tilesReleased = written;
		releaseMutex.unlock();
#endif
	};

	for (unsigned int t = 0; t < options.threadNum; t++) {
		boost::asio::post(pool, [&]() {
			std::size_t startIndex, batchSize;
			uint64_t seq;
			GroupWork &mine = *groupWork[nextWorker++];
			std::vector<std::string> tileTimings;
			std::vector<std::string> coverTiles(config.endZoom + 1);
			std::vector<bool> coverDone(config.endZoom + 1);
			auto beginWriting = [&](uint64_t key) {
				if (toPMTiles) sharedData.pmtiles.batches.begin(key);
				if (toMBTiles) sharedData.mbtiles.batches.begin(key);
			};
			auto endWriting = [&](uint64_t nextKey) {
				if (toPMTiles) { sharedData.pmtiles.batches.setNext(nextKey); sharedData.pmtiles.batches.end(); }
				if (toMBTiles) { sharedData.mbtiles.batches.setNext(nextKey); sharedData.mbtiles.batches.end(); }
			};
			// (the tile is read before it's written: see releaseWrittenTiles)
			auto showProgress = [&](const std::pair<unsigned int, TileCoordinates> &tile) {
				if (io_mutex.try_lock()) {
					uint64_t written = tilesWritten.load();

					if (written >= lastTilesWritten + tileCoordinates.size() / 100 || ISATTY) {
						lastTilesWritten = written;
						// Show progress grouped by z6 (or lower)
						size_t z = tile.first;
						size_t x = tile.second.x;
						size_t y = tile.second.y;
						if (z > CLUSTER_ZOOM) {
							x = x / (1 << (z - CLUSTER_ZOOM));
							y = y / (1 << (z - CLUSTER_ZOOM));
							z = CLUSTER_ZOOM;
						}
						cout << "z" << z << "/" << x << "/" << y << ", writing tile " << written << " of " << tileCoordinates.size() << "               \r" << std::flush;
					}
					io_mutex.unlock();
				}
			};
			auto showTimings = [&]() {
				if (options.logTileTimings) {
					const std::lock_guard<std::mutex> lock(io_mutex);
					std::cout << std::endl;
					for (const auto& output : tileTimings)
						std::cout << output << std::endl;
				}
				tileTimings.clear();
			};
			// The tiles batchBegin .. endIndex-1 (a unit), in a new clip-cache batch or going on with
			// the current one
			auto workOnTiles = [&](std::size_t batchBegin, std::size_t endIndex, bool newCacheBatch, bool noStore) {
				if (newCacheBatch) clipCacheBatchId = ++batchCounter;
				// A single-tile batch has no descendants to reuse its clips.
				clipCacheNoStore = noStore;
				// The covered z12 tile of this unit, and the tiles saved for its descendants per zoom
				bool coverActive = false;
				TileCoordinates coverRoot(0, 0);
				for(std::size_t i = batchBegin; i < endIndex; ++i) {
					// After a failed tile or write, the units left are run through without computing
					// anything, so that the hand-out and the ordered writers still finish
					if (tileFailed || writeFailed()) break;
					unsigned int zoom = tileCoordinates[i].zoom();
					TileCoordinates coords = tileCoordinates[i].coords();

#ifdef CLOCK_MONOTONIC
					timespec start, end;
					if (options.logTileTimings)
						clock_gettime(CLOCK_MONOTONIC, &start);
#endif

					const bool inCovered = coverActive && zoom > coverZoom &&
						(coords.x >> (zoom - coverZoom)) == coverRoot.x && (coords.y >> (zoom - coverZoom)) == coverRoot.y;
					try {
						if (inCovered && coverDone[zoom]) {
							sharedData.pmtiles.saveTile(zoom, coords.x, coords.y, coverTiles[zoom]);
						} else {
							std::vector<std::vector<OutputObjectID>> data;
							for (auto source : sources) {
								data.emplace_back(source->getObjectsForTile(sortOrders, attributeStore, zoom, coords));
							}
							outputProc(sharedData, sources, attributeStore, data, coords, zoom, inCovered ? &coverTiles[zoom] : nullptr);
							if (inCovered) {
								coverDone[zoom] = true;
							} else if (zoom <= coverZoom) {
								coverActive = reuseCovered && zoom == coverZoom && coveredByPolygons(coords);
								if (coverActive) {
									coverRoot = coords;
									std::fill(coverDone.begin(), coverDone.end(), false);
								}
							}
						}
					} catch (const std::exception& e) {
						recordTileError(zoom, coords, e.what());
						break;
					} catch (...) {
						recordTileError(zoom, coords, "unknown exception");
						break;
					}

#ifdef CLOCK_MONOTONIC
					if (options.logTileTimings) {
						clock_gettime(CLOCK_MONOTONIC, &end);
						uint64_t tileNs = 1e9 * (end.tv_sec - start.tv_sec) + end.tv_nsec - start.tv_nsec;
						std::string output = "z" + std::to_string(zoom) + "/" + std::to_string(coords.x) + "/" + std::to_string(coords.y) + " took " + std::to_string(tileNs/1e6) + " ms";
						tileTimings.push_back(output);
					}
#endif
				}
			};

			// The groups handed out to this thread
			while (true) {
				uint64_t needed = UINT64_MAX;
				if (toPMTiles) needed = sharedData.pmtiles.batches.waitForRoom();
				if (toMBTiles) needed = sharedData.mbtiles.batches.waitForRoom();
				const int got = nextBatch(startIndex, batchSize, seq, needed);
				trimReleased();
				if (got == 0) break;
				if (got == 2) continue;
				// Shared pieces for the tiles coming up (see TileDataSource::precomputeSharedPieces)
				if (!tileFailed && !writeFailed()) for (auto source : sources) source->computeSharedPieces(z6HandedOut.load(), pieceBudget);
				{
					// The group's units, open to other threads from now on
					std::lock_guard<std::mutex> lock(mine.m);
					mine.unitStart.clear(); mine.unitKey.clear(); mine.unitNoStore.clear();
					for (std::size_t b = groupStarts[seq]; b < groupStarts[seq + 1]; b++) {
						uint64_t unit = 0;
						for (std::size_t i = batchStarts[b]; i < batchStarts[b + 1]; i++) {
							if (i > batchStarts[b] && tileCoordinates[i].zoom() > cutZoom) continue;
							mine.unitStart.push_back(i);
							mine.unitKey.push_back(((uint64_t)b << 16) | unit++);
							mine.unitNoStore.push_back(batchStarts[b + 1] - batchStarts[b] == 1);
						}
					}
					mine.unitStart.push_back(startIndex + batchSize);
					mine.unitKey.push_back((uint64_t)groupStarts[seq + 1] << 16);
					mine.group = seq;
					mine.front = 0;
					mine.back = mine.unitNoStore.size();
					mine.active = true;
				}
				const std::pair<unsigned int, TileCoordinates> firstTile = tileCoordinates[startIndex];
				// Consecutive units are written as one run
				std::size_t tilesDone = 0;
				bool writing = false;
				std::size_t last = 0;
				while (true) {
					std::size_t u;
					{
						std::lock_guard<std::mutex> lock(mine.m);
						if (mine.front >= mine.back) { mine.active = false; break; }
						u = mine.front++;
					}
					const bool goesOn = writing && u == last + 1;
					if (writing && !goesOn) { endWriting(mine.unitKey[last + 1]); writing = false; }
					if (!writing) { beginWriting(mine.unitKey[u]); writing = true; }
					// a new clip-cache batch at a batch start, and after a gap
					workOnTiles(mine.unitStart[u], mine.unitStart[u + 1], !goesOn || (mine.unitKey[u] & 0xffff) == 0, mine.unitNoStore[u]);
					tilesDone += mine.unitStart[u + 1] - mine.unitStart[u];
					last = u;
				}
				if (writing) endWriting(mine.unitKey[last + 1]);
				tilesWritten += tilesDone;
				showTimings();

				{
					const std::lock_guard<std::mutex> lock(batchMutex);
					// z6 tiles the hand-out has moved past (an early group's may still be ahead of it)
					for (int64_t b : batchZ6)
						if (--z6Running[b] == 0 && b < z6HandedOut) releaseZ6(b);
				}
				trimReleased();
				releaseWrittenTiles();
				showProgress(firstTile);
			}

			// All groups are handed out: take the last units not started yet of the groups still
			// being worked on
			while (true) {
				if (toPMTiles) sharedData.pmtiles.batches.waitForRoom();
				if (toMBTiles) sharedData.mbtiles.batches.waitForRoom();
				std::size_t from = 0, to = 0;
				uint64_t key = 0, nextKey = 0;
				bool noStore = false, found = false;
				{
					// Under batchMutex, so that the owner can't release the unit's z6 tiles meanwhile
					const std::lock_guard<std::mutex> lock(batchMutex);
					batchZ6.clear();
					uint64_t group = 0;
					for (auto &work : groupWork) {
						std::lock_guard<std::mutex> workLock(work->m);
						if (!work->active || work->front >= work->back) continue;
						const std::size_t u = --work->back;
						from = work->unitStart[u]; to = work->unitStart[u + 1];
						key = work->unitKey[u]; nextKey = work->unitKey[u + 1];
						noStore = work->unitNoStore[u];
						group = work->group;
						found = true;
						break;
					}
					if (found && groupCountsZ6[group]) {
						// Its z6 tiles' shared pieces are kept until it is done
						for (std::size_t i = from; i < to; i++) {
							const int64_t z6 = z6Of(i);
							if (z6 >= 0 && (batchZ6.empty() || batchZ6.back() != z6)) batchZ6.push_back(z6);
						}
						for (int64_t z6 : batchZ6) z6Running[z6]++;
					}
				}
				if (!found) break;
				const std::pair<unsigned int, TileCoordinates> firstTile = tileCoordinates[from];
				beginWriting(key);
				workOnTiles(from, to, true, noStore);
				tilesWritten += to - from;
				endWriting(nextKey);
				showTimings();
				{
					const std::lock_guard<std::mutex> lock(batchMutex);
					for (int64_t z6 : batchZ6)
						if (--z6Running[z6] == 0 && z6 < z6HandedOut) releaseZ6(z6);
				}
				trimReleased();
				releaseWrittenTiles();
				showProgress(firstTile);
			}
		});
	}
	// Wait for all tasks in the pool to complete.
	pool.join();
#ifdef __GLIBC__
	// The tiles that were held for the ordered writer are freed, but glibc keeps their pages in its
	// arenas: give them back before close() makes the directories (several GB on a planet)
	malloc_trim(0);
#endif
	if (toPMTiles) sharedData.pmtiles.batches.setFirstUnhanded(nullptr);
	if (toMBTiles) sharedData.mbtiles.batches.setFirstUnhanded(nullptr);
	if (tileFailed) {
		cerr << endl << "Error writing tile " << tileError << endl;
		return -1;
	}
	// A write that failed while the tiles were written (see OrderedBatches::guarded)
	std::exception_ptr writeError = toPMTiles ? sharedData.pmtiles.batches.error() : toMBTiles ? sharedData.mbtiles.batches.error() : nullptr;
	if (writeError) {
		try {
			std::rethrow_exception(writeError);
		} catch (const std::exception& e) {
			cerr << endl << "Error writing " << sharedData.outputFile << ": " << e.what() << endl;
		} catch (...) {
			cerr << endl << "Error writing " << sharedData.outputFile << endl;
		}
		return -1;
	}

	// ----	Close tileset

	try {
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
	} catch (const std::exception& e) {
		cerr << endl << "Error writing " << sharedData.outputFile << ": " << e.what() << endl;
		return -1;
	}

#ifndef _MSC_VER
	if (verbose) {
		struct rusage r_usage;
		getrusage(RUSAGE_SELF, &r_usage);
		cout << "\nMemory used: " << r_usage.ru_maxrss << endl;
	}
#endif

	cout << endl << "Filled the tileset with good things at " << sharedData.outputFile << endl;

	// Everything is written. Freeing the stores one allocation at a time takes a quarter of a minute
	// on a planet build and nothing needs it, so leave without the destructors - unless they still
	// have the --store files to delete.
	if (options.osm.storeFile.empty()) {
		cout.flush();
		std::fflush(nullptr);
#ifdef __linux__
		if (!options.noEarlyExit) exitEarly(options.threadNum);
		releaseMemoryAndExit(options.threadNum);
#else
		std::_Exit(EXIT_SUCCESS);
#endif
	}
}
