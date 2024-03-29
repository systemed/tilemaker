#include <iostream>
#include "external/minunit.h"
#include "options_parser.h"

const char* PROGRAM_NAME = "./tilemaker";
using namespace OptionsParser;

Options parse(std::vector<std::string>& args) {
	const char* argv[100];

	argv[0] = PROGRAM_NAME;
	for(int i = 0; i < args.size(); i++)
		argv[1 + i] = args[i].data();

	return parse(1 + args.size(), argv);
}

#define ASSERT_THROWS(MESSAGE, ...) \
{ \
	std::vector<std::string> args = { __VA_ARGS__ }; \
	bool threw = false; \
	try { \
		auto opts = parse(args); \
	} catch(OptionsParser::OptionException& e) { \
		threw = std::string(e.what()).find(MESSAGE) != std::string::npos; \
	} \
	if (!threw) mu_check((std::string("expected exception with ") + MESSAGE).empty()); \
}

MU_TEST(test_options_parser) {
	// No args is invalid.
	ASSERT_THROWS("You must specify an output file");

	// Output without input is valid
	{
		std::vector<std::string> args = {"--output", "foo.mbtiles"};
		auto opts = parse(args);
		mu_check(opts.inputFiles.size() == 0);
	}

	// You can ask for --help.
	{
		std::vector<std::string> args = {"--help"};
		auto opts = parse(args);
		mu_check(opts.showHelp);
	}

	// Common happy path is output and input
	{
		std::vector<std::string> args = {"--output", "foo.mbtiles", "--input", "ontario.pbf"};
		auto opts = parse(args);
		mu_check(opts.inputFiles.size() == 1);
		mu_check(opts.inputFiles[0] == "ontario.pbf");
		mu_check(opts.outputFile == "foo.mbtiles");
		mu_check(opts.outputMode == OutputMode::MBTiles);
		mu_check(!opts.osm.materializeGeometries);
		mu_check(!opts.osm.shardStores);
	}

	// --fast without store should have materialized geometries
	{
		std::vector<std::string> args = {"--output", "foo.mbtiles", "--input", "ontario.pbf", "--fast"};
		auto opts = parse(args);
		mu_check(opts.inputFiles.size() == 1);
		mu_check(opts.inputFiles[0] == "ontario.pbf");
		mu_check(opts.outputFile == "foo.mbtiles");
		mu_check(opts.outputMode == OutputMode::MBTiles);
		mu_check(opts.osm.materializeGeometries);
		mu_check(!opts.osm.shardStores);
	}

	// --store should optimize for reduced memory
	{
		std::vector<std::string> args = {"--output", "foo.mbtiles", "--input", "ontario.pbf", "--store", "/tmp/store"};
		auto opts = parse(args);
		mu_check(opts.inputFiles.size() == 1);
		mu_check(opts.inputFiles[0] == "ontario.pbf");
		mu_check(opts.outputFile == "foo.mbtiles");
		mu_check(opts.outputMode == OutputMode::MBTiles);
		mu_check(opts.osm.storeFile == "/tmp/store");
		mu_check(!opts.osm.materializeGeometries);
		mu_check(opts.osm.shardStores);
	}

	// --store --fast should optimize for speed
	{
		std::vector<std::string> args = {"--output", "foo.pmtiles", "--input", "ontario.pbf", "--store", "/tmp/store", "--fast"};
		auto opts = parse(args);
		mu_check(opts.inputFiles.size() == 1);
		mu_check(opts.inputFiles[0] == "ontario.pbf");
		mu_check(opts.outputFile == "foo.pmtiles");
		mu_check(opts.outputMode == OutputMode::PMTiles);
		mu_check(opts.osm.storeFile == "/tmp/store");
		mu_check(!opts.osm.materializeGeometries);
		mu_check(!opts.osm.shardStores);
	}

	// Two input files implies --materialize
	{
		std::vector<std::string> args = {"--output", "foo.mbtiles", "--input", "ontario.pbf", "--input", "alberta.pbf"};
		auto opts = parse(args);
		mu_check(opts.inputFiles.size() == 2);
		mu_check(opts.inputFiles[0] == "ontario.pbf");
		mu_check(opts.inputFiles[1] == "alberta.pbf");
		mu_check(opts.outputFile == "foo.mbtiles");
		mu_check(opts.outputMode == OutputMode::MBTiles);
		mu_check(opts.osm.materializeGeometries);
		mu_check(!opts.osm.shardStores);
	}

	ASSERT_THROWS("Couldn't open .json config", "--input", "foo", "--output", "bar", "--config", "nonexistent-config.json");
	ASSERT_THROWS("Couldn't open .lua script", "--input", "foo", "--output", "bar", "--process", "nonexistent-script.lua");
}

MU_TEST_SUITE(test_suite_options_parser) {
	MU_RUN_TEST(test_options_parser);
}

int main() {
	MU_RUN_SUITE(test_suite_options_parser);
	MU_REPORT();
	return MU_EXIT_CODE;
}
