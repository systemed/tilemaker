#ifndef OPTIONS_PARSER_H
#define OPTIONS_PARSER_H

#include <cstdint>
#include <exception>
#include <string>
#include <vector>

namespace OptionsParser {
	struct OptionException : std::exception {
		OptionException(std::string message): message(message) {}

		/// Returns the explanatory string.
		const char* what() const noexcept override {
				return message.data();
		}

		private:
			std::string message;
	};

	enum class OutputMode: char { File = 0, MBTiles = 1, PMTiles = 2 };

	struct OsmOptions {
		std::string storeFile;
		bool fast = false;
		bool compact = false;
		bool skipIntegrity = false;
		bool uncompressedNodes = false;
		bool uncompressedWays = false;
		bool materializeGeometries = false;
		// lazyGeometries is the inverse of materializeGeometries. It can be passed
		// to override an implicit materializeGeometries, as in the non-store case.
		bool lazyGeometries = false;
		bool shardStores = false;
	};

	struct Options {
		std::vector<std::string> inputFiles;
		std::string luaFile;
		std::string jsonFile;
		uint32_t threadNum = 0;
		std::string outputFile;
		std::string bbox;

		OsmOptions osm;
		bool showHelp = false;
		bool verbose = false;
		bool mergeSqlite = false;
		OutputMode outputMode = OutputMode::File;
		bool logTileTimings = false;
	};

	Options parse(const int argc, const char* argv[]);
	void showHelp();
};

#endif
