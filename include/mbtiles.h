/*! \file */ 
#ifndef _MBTILES_H
#define _MBTILES_H

#include <string>
#include <mutex>
#include <vector>
#include "external/sqlite_modern_cpp.h"

struct PendingStatement {
	int zoom;
	int x;
	int y;
	std::string data;
	bool isMerge;
};

/** \brief Write to MBTiles (sqlite) database
*
* (note that sqlite_modern_cpp.h is very slightly changed from the original, for blob support and an .init method)
*/
class MBTiles { 
	sqlite::database db;
	std::vector<sqlite::database_binder> preparedStatements;
	std::mutex m;
	bool inTransaction;

	std::shared_ptr<std::vector<PendingStatement>> pendingStatements1, pendingStatements2;
	std::mutex pendingStatementsMutex;

	void insertOrReplace(int zoom, int x, int y, const std::string& data, bool isMerge);
	void flushPendingStatements();

public:
	MBTiles();
	virtual ~MBTiles();
	void openForWriting(std::string &filename);
	void writeMetadata(std::string key, std::string value);
	void saveTile(int zoom, int x, int y, std::string *data, bool isMerge);
	void closeForWriting();

	void openForReading(std::string &filename);
	void readBoundingBox(double &minLon, double &maxLon, double &minLat, double &maxLat);
	void readTileList(std::vector<std::tuple<int,int,int>> &tileList);
	std::vector<char> readTile(int zoom, int col, int row);
	bool readTileAndUncompress(std::string &data, int zoom, int col, int row, bool isCompressed, bool asGzip);
};

#endif //_MBTILES_H

