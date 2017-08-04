
#ifndef _MBTILES_H
#define _MBTILES_H

#include <string>
#include <mutex>
#include "sqlite_modern_cpp.h"

/** \brief Write to MBTiles (sqlite) database
*
* (note that sqlite_modern_cpp.h is very slightly changed from the original, for blob support and an .init method)
*/
class MBTiles
{ 
	sqlite::database db;
	std::mutex m;

public:
	MBTiles();
	virtual ~MBTiles();
	void open(std::string *filename);
	void writeMetadata(std::string key, std::string value);
	void saveTile(int zoom, int x, int y, std::string *data);
};

#endif //_MBTILES_H

