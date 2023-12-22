/*! \file */ 
#ifndef _PMTILES_H
#define _PMTILES_H

#include <fstream>
#include "external/pmtiles.hpp"

struct TileOffset {
	uint64_t offset : 40;
	size_t length : 24;

	TileOffset(uint64_t offset, size_t length) : offset(offset),length(length) { }
	TileOffset();
};

// Maximum number of tiles in a leaf directory
#define LEAF_DIRECTORY_SIZE 10000000
// Combined size of header and root directory (= start of tile data)
#define HEADER_ROOT 16384
// Tile ID at which to start using leaf directories (=z6/0/0)
#define FIRST_LEAF_TILE 1365
// Threshold for using the root directory only
#define ROOT_ONLY 2200
// Maximum size in bytes of tiles considered "tiny" (i.e. potentially repeatable)
#define TINY_LENGTH 100
// Expire the tiny cache when it reaches this size
#define TINY_MAX_SIZE 10000

class PMTiles { 

public:
	PMTiles();
	virtual ~PMTiles();

	pmtiles::headerv3 header;
	bool isSparse = true;

	void open(std::string &filename);
	void saveTile(int zoom, int x, int y, std::string &data);
	void close(std::string &metadata);

private:
	std::ofstream outputStream;
	std::mutex fileMutex;	// guards file writes, numTilesWritten
	std::mutex indexMutex;	// guards access to sparseIndex, denseIndex, tinyCache, numTilesAddressed
	uint64_t leafStart = 0;
	uint64_t numTilesWritten = 0;
	uint64_t numTilesAddressed = 0;
	uint64_t numTileEntries = 0;
	std::map<uint64_t, TileOffset> sparseIndex;
	std::vector<TileOffset> denseIndex;
	std::unordered_map<std::string, TileOffset> tinyCache;

	void appendWithRLE(std::vector<pmtiles::entryv3> &entries, pmtiles::entryv3 &entry);
	void appendTileEntry(uint64_t tileId, TileOffset offset, std::vector<pmtiles::entryv3> &rootEntries, std::vector<pmtiles::entryv3> &entries);
	void flushEntries(std::vector<pmtiles::entryv3> &rootEntries, std::vector<pmtiles::entryv3> &entries);
};

#endif //_PMTILES_H
