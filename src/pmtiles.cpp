#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <thread>

#include "pmtiles.h"
#include "helpers.h"

TileOffset::TileOffset() { }
PMTiles::PMTiles() { }
PMTiles::~PMTiles() { }

void PMTiles::open(std::string &filename) {
	std::cout << "Creating pmtiles at " << filename << std::endl;
	outputStream.open(filename, std::ios::out | std::ios::trunc | std::ios::binary);
	// dummy header/root directory for now - we'll write it all later
	char header[HEADER_ROOT] = "PMTiles";
	outputStream.write(header, HEADER_ROOT);
}

// Finish writing the .pmtiles file
void PMTiles::close(std::string &metadata) {
	std::cout << "\nClosing pmtiles file" << std::flush;

	// Add all tiles to directories: low zooms to the root, the rest to leaf directories of up to
	// LEAF_DIRECTORY_SIZE entries. The leaves are serialized and compressed on all cores (one after
	// the other that took ~9 s for the planet), then written in order, so the file is the same.
	std::vector<pmtiles::entryv3> rootEntries;
	std::vector<std::vector<pmtiles::entryv3>> leaves(1);
	auto append = [&](uint64_t tileId, TileOffset offset) {
		pmtiles::entryv3 entry = pmtiles::entryv3(tileId, offset.offset, offset.length, 1); // 1=RLE
		if (tileId<FIRST_LEAF_TILE) {
			// <z6 so root directory
			appendWithRLE(rootEntries, entry);
		} else {
			appendWithRLE(leaves.back(), entry);
			if (leaves.back().size()>=LEAF_DIRECTORY_SIZE) leaves.emplace_back();
		}
	};
	if (isSparse) {
		for (auto it : sparseIndex) append(it.first, it.second);
	} else {
		for (size_t tileId=0; tileId<denseIndex.size(); tileId++) {
			if (denseIndex[tileId].length != 0xffffff) append(tileId, denseIndex[tileId]);
		}
	}
	if (leaves.back().empty()) leaves.pop_back();
	leafStart = outputStream.tellp();
	if (numTileEntries < ROOT_ONLY) {
		for (auto const &leaf : leaves) rootEntries.insert(rootEntries.end(), leaf.begin(), leaf.end());
	} else {
		std::vector<std::string> compressedLeaves(leaves.size());
		std::atomic<size_t> next(0);
		auto compressLeaves = [&]() {
			for (size_t i = next++; i < leaves.size(); i = next++)
				compressedLeaves[i] = compress_string(pmtiles::serialize_directory(leaves[i]), Z_DEFAULT_COMPRESSION, true);
		};
		std::vector<std::thread> threads;
		const size_t numThreads = std::min<size_t>(leaves.size(), std::max(1u, std::thread::hardware_concurrency()));
		for (size_t t = 0; t < numThreads; t++) threads.emplace_back(compressLeaves);
		for (auto &t : threads) t.join();
		for (size_t i = 0; i < leaves.size(); i++) {
			// write the leaf directory, and add a reference to it to the root directory
			const uint64_t location = outputStream.tellp();
			outputStream.write(compressedLeaves[i].c_str(), compressedLeaves[i].size());
			rootEntries.emplace_back(pmtiles::entryv3(leaves[i][0].tile_id, location-leafStart, compressedLeaves[i].size(), 0));
		}
	}
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
	header.tile_type = tileType;

	// write header
	std::string headerString = header.serialize();
	outputStream.seekp(0);
	outputStream.write(headerString.c_str(), headerString.size());

	// ...and we're done!
	outputStream.close();
}

// Add an entry either to the current leaf directory, or (for lowzoom) the root directory
// Handle run-length encoding for the directory entries
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
		outputStream.write(compressed.c_str(), compressed.size());
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
