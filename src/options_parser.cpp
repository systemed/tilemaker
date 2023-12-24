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
		("store",  po::value< string >(&options.osm.storeFile),  "temporary storage for node/ways/relations data")
		("compact",po::bool_switch(&options.osm.compact),  "Reduce overall memory usage (compact mode).\nNOTE: This requires the input to be renumbered (osmium renumber)")
		("no-compress-nodes", po::bool_switch(&options.osm.uncompressedNodes),  "Store nodes uncompressed")
		("no-compress-ways", po::bool_switch(&options.osm.uncompressedWays),  "Store ways uncompressed")
		("materialize-geometries", po::bool_switch(&options.osm.materializeGeometries),  "Materialize geometries - faster, but requires more memory")
		("shard-stores", po::bool_switch(&options.osm.shardStores),  "Shard stores - use an alternate reading/writing strategy for low-memory machines")
		("verbose",po::bool_switch(&options.verbose),                                   "verbose error output")
		("skip-integrity",po::bool_switch(&options.osm.skipIntegrity),                       "don't enforce way/node integrity")
		("log-tile-timings", po::bool_switch(&options.logTileTimings), "log how long each tile takes")
		("threads",po::value< uint >(&options.threadNum)->default_value(0),              "number of threads (automatically detected if 0)");
	po::options_description performance("Performance options");
	performance.add_options()
			("help-module", po::value<std::string>(),
					"produce a help for a given module")
			("version", "output the version number")
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
	
	if (vm.count("help")) {
		options.showHelp = true;
		return options;
	}
	if (vm.count("output") == 0) {
		throw OptionException{ "You must specify an output file or directory. Run with --help to find out more." };
	}

	if (vm.count("input") == 0) {
		throw OptionException{ "No source .osm.pbf file supplied" };
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

	return options;
}
