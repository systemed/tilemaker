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

#define LEAF_DIRECTORY_SIZE 10000000

class PMTiles { 
//	sqlite::database db;
//	std::vector<sqlite::database_binder> preparedStatements;
//	std::mutex m;
//	bool inTransaction;

//	std::shared_ptr<std::vector<PendingStatement>> pendingStatements1, pendingStatements2;
//	std::mutex pendingStatementsMutex;

//	void insertOrReplace(int zoom, int x, int y, const std::string& data, bool isMerge);
//	void flushPendingStatements();

public:
	PMTiles();
	virtual ~PMTiles();

	pmtiles::headerv3 header;

	void open(std::string &filename);
//	void writeMetadata(std::string key, std::string value);
	void saveTile(int zoom, int x, int y, std::string &data);
	void close(std::string &metadata);

private:
	std::ofstream outputStream;
	std::mutex fileMutex;
	bool isSparse = true;
	uint64_t leafStart = 0;
	uint64_t numTilesWritten = 0;
	std::map<uint64_t, TileOffset> sparseIndex;
	std::vector<TileOffset> denseIndex;

	void appendTileEntry(uint64_t tileId, TileOffset offset, std::vector<pmtiles::entryv3> &rootEntries, std::vector<pmtiles::entryv3> &entries);
	void flushEntries(std::vector<pmtiles::entryv3> &rootEntries, std::vector<pmtiles::entryv3> &entries);
};

#endif //_PMTILES_H
