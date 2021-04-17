/*! \file */ 
#ifndef _MBTILES_H
#define _MBTILES_H

#include <string>
#include <mutex>
#include <vector>
#include "sqlite_modern_cpp.h"

/** \brief Write to MBTiles (sqlite) database
*
* (note that sqlite_modern_cpp.h is very slightly changed from the original, for blob support and an .init method)
*/
class MBTiles { 
	sqlite::database db;
	std::mutex m;
	bool inTransaction;

public:
	MBTiles();
	virtual ~MBTiles();
	void openForWriting(std::string *filename);
	void writeMetadata(std::string key, std::string value);
	void saveTile(int zoom, int x, int y, std::string *data);
	void closeForWriting();

	void openForReading(std::string *filename);
	void readBoundingBox(double &minLon, double &maxLon, double &minLat, double &maxLat);
	void readTileList(std::vector<std::tuple<int,int,int>> &tileList);
	std::vector<char> readTile(int zoom, int col, int row);
	bool readTileAndUncompress(std::string &data, int zoom, int col, int row);
};

#endif //_MBTILES_H

