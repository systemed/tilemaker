#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <mutex>

#include "pmtiles.h"
#include "external/pmtiles.hpp"

/*
	Future enhancements:
	- order tile-writing so it's in tileid order
	- run-length encoding for sea tiles etc. [would be better to do this before compressing the tile]

	Useful stuff in pmtiles.hpp:
	- pmtiles::TILETYPE_MVT = 1
	- pmtiles::[compression types]
	- pmtiles::serialize (headerv3 object)
	- structs:
		pmtiles::zxy
		pmtiles::entryv3
	- pmtiles::zxy_to_tileid
	- pmtiles::serialize_directory
	- pmtiles::build_root_leaves (what does this do?)
	- pmtiles::make_root_leaves (? - calls build_root_leaves)

const tzValues: number[] = [
  z0 z1 z2 z3  z4  z5   z6    z7    z8
  0, 1, 5, 21, 85, 341, 1365, 5461, 21845, 87381, 349525, 1398101, 5592405,
  22369621, 89478485, 357913941, 1431655765, 5726623061, 22906492245,
  91625968981, 366503875925, 1466015503701, 5864062014805, 23456248059221,
  93824992236885, 375299968947541, 1501199875790165,
];

*/

TileOffset::TileOffset() { }
PMTiles::PMTiles() { }
PMTiles::~PMTiles() { }

void PMTiles::open(std::string &filename) {
	std::cout << "Opening PMTiles file " << filename << std::endl;
	outputStream.open(filename);
	// **** set isSparse according to the number of tiles
	// dummy header/root directory for now - we'll write it all later
	char header[16384] = "PMTiles";
	outputStream.write(header, 16384);
}

void PMTiles::close() {
	std::cout << "Closing PMTiles file" << std::endl;
	std::vector<pmtiles::entryv3> rootEntries;
	std::vector<pmtiles::entryv3> entries;
	if (isSparse) {
		for (auto it : sparseIndex) {
			appendTileEntry(it.first, it.second, rootEntries, entries);
		}
	} else {
		for (size_t tileId=0; tileId<denseIndex.size(); tileId++) {
			if (denseIndex[tileId].length != 0) appendTileEntry(tileId, denseIndex[tileId], rootEntries, entries);
		}
	}
	flushEntries(rootEntries,entries);
	// write leaf directories
	// create 127-byte header
	// create root directory
	// create JSON metadata
	outputStream.close();
}

void PMTiles::appendTileEntry(uint64_t tileId, TileOffset offset, std::vector<pmtiles::entryv3> &rootEntries, std::vector<pmtiles::entryv3> &entries) {
	pmtiles::entryv3 entry = pmtiles::entryv3(tileId, offset.offset, offset.length, 1); // 1=RLE
	if (tileId<1365) {
		// <z6 so root directory
		rootEntries.emplace_back(entry);
	} else {
		entries.emplace_back(entry);
		if (entries.size()>=LEAF_DIRECTORY_SIZE) flushEntries(rootEntries, entries);
	}
}
void PMTiles::flushEntries(std::vector<pmtiles::entryv3> &rootEntries, std::vector<pmtiles::entryv3> &entries) {
	if (entries.size()==0) return;
	uint64_t startId = entries[0].tile_id;
	std::string directory = pmtiles::serialize_directory(entries);
	// write the leaf directory to disk
	std::lock_guard<std::mutex> lock(fileMutex);
	uint64_t location = outputStream.tellp();
	uint64_t length = directory.size();
	outputStream << directory;
	// append reference to the root directory
	pmtiles::entryv3 rootEntry = pmtiles::entryv3(startId, location, length, 0);
	rootEntries.emplace_back(rootEntry);
}

void PMTiles::saveTile(int zoom, int x, int y, std::string &data) {
	uint64_t tileId = pmtiles::zxy_to_tileid(zoom,x,y);
	std::cout << "Writing tile " << zoom << "/" << x << "/" << y << " = " << tileId << std::endl;
	std::lock_guard<std::mutex> lock(fileMutex);
	// write to file
	TileOffset offset = TileOffset(static_cast<uint64_t>(outputStream.tellp()), data.size());
	outputStream << data;
	// store in index
	if (isSparse) {
		sparseIndex[tileId] = offset;
	} else {
		if (tileId>denseIndex.size()) denseIndex.resize(tileId+1000);
		denseIndex[tileId] = offset;
	}
}
