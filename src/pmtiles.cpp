#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <mutex>

#include "pmtiles.h"
#include "helpers.h"
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

// Finish writing the .pmtiles file
void PMTiles::close(std::string &metadata) {
	std::cout << "Closing PMTiles file" << std::endl;

	// add all tiles to directories, writing leaf directories as we go
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
	uint64_t leafLength = static_cast<uint64_t>(outputStream.tellp()) - leafStart;

	// create JSON metadata
	std::string compressed = compress_string(metadata, Z_DEFAULT_COMPRESSION, true);
	uint64_t jsonStart = static_cast<uint64_t>(outputStream.tellp());
	int jsonLength = compressed.size();
	outputStream.write(compressed.c_str(), jsonLength);
	std::cout << "Written JSON metadata at " << jsonStart << " for " << jsonLength << std::endl;
	
	// write root directory
	std::string directory = pmtiles::serialize_directory(rootEntries);
	compressed = compress_string(directory, Z_DEFAULT_COMPRESSION, true);
	int rootLength = compressed.size();
	if (rootLength > 16257) { throw std::runtime_error(".pmtiles root directory was too large - please file an issue"); }
	std::cout << "Root directory has " << rootEntries.size() << " entries and is " << rootLength << " bytes" << std::endl;
	outputStream.seekp(127);
	outputStream.write(compressed.c_str(), rootLength);

	// add all sizes to 127-byte header
	header.root_dir_offset = 127;
	header.root_dir_bytes = rootLength;
	header.json_metadata_offset = jsonStart;
	header.json_metadata_bytes = jsonLength;
	header.leaf_dirs_offset = leafStart;
	header.leaf_dirs_bytes = leafLength;
	header.tile_data_offset = 16384;
	header.tile_data_bytes = leafStart - 16384;
	header.addressed_tiles_count = numTilesWritten; // TODO - change this when we use RLE
	header.tile_entries_count = numTilesWritten; // TODO - change this when we use RLE
	header.tile_contents_count = numTilesWritten;
	header.clustered = false;
	header.internal_compression = pmtiles::COMPRESSION_GZIP;
	header.tile_compression = pmtiles::COMPRESSION_GZIP;
	header.tile_type = pmtiles::TILETYPE_MVT;

	// write header
	std::string headerString = header.serialize();
	outputStream.seekp(0);
	outputStream.write(headerString.c_str(), headerString.size());

	// ...and we're done!
	outputStream.close();
}

// Add an entry either to the current leaf directory, or (for lowzoom) the root directory
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

// Write a leaf directory to file, and add a reference to it to the root directory
void PMTiles::flushEntries(std::vector<pmtiles::entryv3> &rootEntries, std::vector<pmtiles::entryv3> &entries) {
	std::cout << "Flushing leaf directory with " << entries.size() << " entries" << std::endl;
	if (entries.size()==0) return;
	uint64_t startId = entries[0].tile_id;
	std::string directory = pmtiles::serialize_directory(entries);
	std::string compressed = compress_string(directory, Z_DEFAULT_COMPRESSION, true);

	// write the leaf directory to disk
	std::lock_guard<std::mutex> lock(fileMutex);
	uint64_t location = outputStream.tellp();
	if (leafStart==0) leafStart=location;
	uint64_t length = compressed.size();
	outputStream << compressed;
	std::cout << "Written leaf directory starting at " << startId << " at " << location << " for " << length << std::endl;

	// append reference to the root directory
	pmtiles::entryv3 rootEntry = pmtiles::entryv3(startId, location, length, 0);
	rootEntries.emplace_back(rootEntry);
}

// Write a tile to file and store it in the index
void PMTiles::saveTile(int zoom, int x, int y, std::string &data) {
	uint64_t tileId = pmtiles::zxy_to_tileid(zoom,x,y);
//	std::cout << "Writing tile " << zoom << "/" << x << "/" << y << " = " << tileId << std::endl;
	std::lock_guard<std::mutex> lock(fileMutex);
	// write to file
	TileOffset offset = TileOffset(static_cast<uint64_t>(outputStream.tellp()), data.size());
	outputStream << data;
	numTilesWritten++;
	// store in index
	if (isSparse) {
		sparseIndex[tileId] = offset;
	} else {
		if (tileId>denseIndex.size()) denseIndex.resize(tileId+1000);
		denseIndex[tileId] = offset;
	}
}
