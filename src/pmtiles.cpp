#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <mutex>

#include "pmtiles.h"
#include "helpers.h"
#include "external/pmtiles.hpp"

/*
	Future enhancements:
	- order tile-writing so it's in tileid order
	- store up writes as per mbtiles.cpp?
*/

TileOffset::TileOffset() { }
PMTiles::PMTiles() { }
PMTiles::~PMTiles() { }

void PMTiles::open(std::string &filename) {
	std::cout << "Creating pmtiles at " << filename << std::endl;
	outputStream.open(filename);
	// dummy header/root directory for now - we'll write it all later
	char header[HEADER_ROOT] = "PMTiles";
	outputStream.write(header, HEADER_ROOT);
}

// Finish writing the .pmtiles file
void PMTiles::close(std::string &metadata) {
	std::cout << "\nClosing pmtiles file" << std::flush;

	// add all tiles to directories, writing leaf directories as we go
	std::vector<pmtiles::entryv3> rootEntries;
	std::vector<pmtiles::entryv3> entries;
	if (isSparse) {
		for (auto it : sparseIndex) {
			appendTileEntry(it.first, it.second, rootEntries, entries);
		}
	} else {
		for (size_t tileId=0; tileId<denseIndex.size(); tileId++) {
			if (denseIndex[tileId].length != 0xffffff) appendTileEntry(tileId, denseIndex[tileId], rootEntries, entries);
		}
	}
	flushEntries(rootEntries,entries);
	uint64_t leafLength = static_cast<uint64_t>(outputStream.tellp()) - leafStart;

	// create JSON metadata
	std::string compressed = compress_string(metadata, Z_DEFAULT_COMPRESSION, true);
	uint64_t jsonStart = static_cast<uint64_t>(outputStream.tellp());
	int jsonLength = compressed.size();
	outputStream.write(compressed.c_str(), jsonLength);
	
	// write root directory
	std::string directory = pmtiles::serialize_directory(rootEntries);
	compressed = compress_string(directory, Z_DEFAULT_COMPRESSION, true);
	int rootLength = compressed.size();
	if (rootLength > (HEADER_ROOT-127)) { throw std::runtime_error(".pmtiles root directory was too large - please file an issue"); }
	outputStream.seekp(127);
	outputStream.write(compressed.c_str(), rootLength);

	// add all sizes to 127-byte header
	header.root_dir_offset = 127;
	header.root_dir_bytes = rootLength;
	header.json_metadata_offset = jsonStart;
	header.json_metadata_bytes = jsonLength;
	header.leaf_dirs_offset = leafStart;
	header.leaf_dirs_bytes = leafLength;
	header.tile_data_offset = HEADER_ROOT;
	header.tile_data_bytes = leafStart - HEADER_ROOT;
	header.addressed_tiles_count = numTilesAddressed;
	header.tile_entries_count = numTileEntries;
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
	if (tileId<FIRST_LEAF_TILE) {
		// <z6 so root directory
		appendWithRLE(rootEntries, entry);
	} else {
		appendWithRLE(entries, entry);
		if (entries.size()>=LEAF_DIRECTORY_SIZE) flushEntries(rootEntries, entries);
	}
}

// Handle run-length encoding for appendTileEntry
void PMTiles::appendWithRLE(std::vector<pmtiles::entryv3> &entries, pmtiles::entryv3 &entry) {
	if (entries.empty() ||
		entries.back().offset != entry.offset ||
		entries.back().tile_id != (entry.tile_id-entries.back().run_length)) { 
			entries.emplace_back(entry); 
			numTileEntries++;
			return;
	}
	entries.back().run_length++;
}

// Write a leaf directory to file, and add a reference to it to the root directory
void PMTiles::flushEntries(std::vector<pmtiles::entryv3> &rootEntries, std::vector<pmtiles::entryv3> &entries) {
	if (entries.size()==0) return;
	uint64_t startId = entries[0].tile_id;
	std::string directory = pmtiles::serialize_directory(entries);
	std::string compressed = compress_string(directory, Z_DEFAULT_COMPRESSION, true);
	entries.clear();

	// write the leaf directory to disk
	std::lock_guard<std::mutex> lock(fileMutex);
	uint64_t location = outputStream.tellp();
	if (leafStart==0) leafStart=location;
	uint64_t length = compressed.size();
	outputStream << compressed;

	// append reference to the root directory
	pmtiles::entryv3 rootEntry = pmtiles::entryv3(startId, location-leafStart, length, 0);
	rootEntries.emplace_back(rootEntry);
}

// Write a tile to file and store it in the index
// - if the tile is small and has already been written, reuse that instead
// - we're a bit fussy about mutexs because compress_string is expensive
void PMTiles::saveTile(int zoom, int x, int y, std::string &data) {
	TileOffset offset;
	bool isNew = false;
	uint64_t tileId = pmtiles::zxy_to_tileid(zoom,x,y);

	// if it's a tiny tile (e.g. sea), see if we've written it already
	std::unique_lock<std::mutex> indexLock1(indexMutex);
	if (data.size()<TINY_LENGTH && tinyCache.find(data) != tinyCache.end()) {
		offset = tinyCache[data];
		indexLock1.unlock();

	// otherwise, compress it and write
	} else {
		indexLock1.unlock();
		std::string compressed = compress_string(data, Z_DEFAULT_COMPRESSION, true);
		std::lock_guard<std::mutex> lock(fileMutex);
		// write to file
		offset = TileOffset(static_cast<uint64_t>(outputStream.tellp()) - HEADER_ROOT, compressed.size());
		outputStream << compressed;
		numTilesWritten++;
		isNew = true;
	}
	
	// store in index
	std::lock_guard<std::mutex> indexLock2(indexMutex);
	numTilesAddressed++;
	if (isSparse) {
		sparseIndex[tileId] = offset;
	} else {
		if (tileId >= denseIndex.size()) denseIndex.resize(tileId+10000, { 0, 0xffffff });
		denseIndex[tileId] = offset;
	}

	// add to cache if tiny
	if (isNew && data.size()<TINY_LENGTH) {
		if (tinyCache.size()>TINY_MAX_SIZE) tinyCache.clear();
		tinyCache.insert({ data, offset });
	}
}
