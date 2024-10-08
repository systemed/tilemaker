#include "options_parser.h"

#include <thread>
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <iostream>
#include "helpers.h"

#ifndef TM_VERSION
#define TM_VERSION (version not set)
#endif
#define STR1(x)  #x
#define STR(x)  STR1(x)

using namespace std;
namespace po = boost::program_options;

po::options_description getParser(OptionsParser::Options& options) {
	po::options_description desc("tilemaker " STR(TM_VERSION) "\nConvert OpenStreetMap .pbf files into vector tiles\n\nAvailable options");
	desc.add_options()
		("help",                                                                 "show help message")
		("input",  po::value< vector<string> >(&options.inputFiles),                     "source .osm.pbf file")
		("output", po::value< string >(&options.outputFile),                             "target directory or .mbtiles/.pmtiles file")
		("bbox",   po::value< string >(&options.bbox),                                   "bounding box to use if input file does not have a bbox header set, example: minlon,minlat,maxlon,maxlat")
		("merge"  ,po::bool_switch(&options.mergeSqlite),                                "merge with existing .mbtiles (overwrites otherwise)")
		("config", po::value< string >(&options.jsonFile)->default_value("config.json"), "config JSON file")
		("process",po::value< string >(&options.luaFile)->default_value("process.lua"),  "tag-processing Lua file")
		("quiet",  po::bool_switch(&options.quiet),                                      "quiet, suppress standard output")
		("verbose",po::bool_switch(&options.verbose),                                   "verbose error output")
		("skip-integrity",po::bool_switch(&options.osm.skipIntegrity),                       "don't enforce way/node integrity")
		("log-tile-timings", po::bool_switch(&options.logTileTimings), "log how long each tile takes");
	po::options_description performance("Performance options");
	performance.add_options()
		("store",  po::value< string >(&options.osm.storeFile),  "temporary storage for node/ways/relations data")
		("fast",   po::bool_switch(&options.osm.fast), "prefer speed at the expense of memory")
		("compact",po::bool_switch(&options.osm.compact),  "use faster data structure for node lookups\nNOTE: This requires the input to be renumbered (osmium renumber)")
		("no-compress-nodes", po::bool_switch(&options.osm.uncompressedNodes),  "store nodes uncompressed")
		("no-compress-ways", po::bool_switch(&options.osm.uncompressedWays),  "store ways uncompressed")
		("materialize-geometries", po::bool_switch(&options.osm.materializeGeometries),  "materialize geometries; uses more memory")
		("shard-stores", po::bool_switch(&options.osm.shardStores),  "use an alternate reading/writing strategy for low-memory machines")
		("threads",po::value<uint32_t>(&options.threadNum)->default_value(0),              "number of threads (automatically detected if 0)")
			;

	desc.add(performance);
	return desc;
}

void OptionsParser::showHelp() {
	Options options;
	auto parser = getParser(options);
	std::cout << parser << std::endl;
}

OptionsParser::Options OptionsParser::parse(const int argc, const char* argv[]) {
	Options options;

	po::options_description desc = getParser(options);
	po::positional_options_description p;
	p.add("input", 1).add("output", 1);

	po::variables_map vm;
	try {
		po::store(po::command_line_parser(argc, argv).options(desc).positional(p).run(), vm);
	} catch (const po::unknown_option& ex) {
		throw OptionException{"Unknown option: " + ex.get_option_name()};
	}
	po::notify(vm);

	if (options.osm.storeFile.empty()) {
		// we're running in memory (no --store)
		// so we default to lazy geometries, but --fast switches to materialized
		if (options.osm.fast) {
			options.osm.materializeGeometries = true;
		}
	} else {
		// we're running on-disk (--store)
		// so we default to sharded, but --fast switches to unsharded
		if (!options.osm.fast) {
			options.osm.shardStores = true;
		}
	}

	if (vm.count("help")) {
		options.showHelp = true;
		return options;
	}
	if (vm.count("output") == 0) {
		throw OptionException{ "You must specify an output file or directory. Run with --help to find out more." };
	}

	if (ends_with(options.outputFile, ".mbtiles") || ends_with(options.outputFile, ".sqlite")) {
		options.outputMode = OutputMode::MBTiles;
	} else if (ends_with(options.outputFile, ".pmtiles")) {
		options.outputMode = OutputMode::PMTiles;
	}

	if (options.threadNum == 0) {
		options.threadNum = max(thread::hardware_concurrency(), 1u);
	}

	// ---- Check config
	if (!boost::filesystem::exists(options.jsonFile)) {
		throw OptionException{ "Couldn't open .json config: " + options.jsonFile };
	}
	if (!boost::filesystem::exists(options.luaFile)) {
		throw OptionException{"Couldn't open .lua script: " + options.luaFile };
	}

	// The lazy geometry code has assumptions that break when more than one
	// input file is used.
	if (options.inputFiles.size() > 1)
		options.osm.materializeGeometries = true;

	return options;
}
