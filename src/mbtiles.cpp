
#include "mbtiles.h"
#include <cmath>
using namespace sqlite;
using namespace std;

MBTiles::MBTiles() {}

MBTiles::~MBTiles() {
		if (db) db << "COMMIT;"; // commit all the changes if open
}

void MBTiles::open(string *filename) {
	db.init(*filename);
	db << "PRAGMA synchronous = OFF;";
	db << "PRAGMA application_id = 0x4d504258;";
	db << "CREATE TABLE IF NOT EXISTS metadata (name text, value text, UNIQUE (name));";
	db << "CREATE TABLE IF NOT EXISTS tiles (zoom_level integer, tile_column integer, tile_row integer, tile_data blob, UNIQUE (zoom_level, tile_column, tile_row));";
	db << "BEGIN;"; // begin a transaction
}
	
void MBTiles::writeMetadata(string key, string value) {
	m.lock();
	db << "REPLACE INTO metadata (name,value) VALUES (?,?);" << key << value;
	m.unlock();
}
	
void MBTiles::saveTile(int zoom, int x, int y, string *data) {
	int tmsY = pow(2,zoom) - 1 - y;
	m.lock();
	db << "REPLACE INTO tiles (zoom_level, tile_column, tile_row, tile_data) VALUES (?,?,?,?);" << zoom << x << tmsY && *data;
	m.unlock();
}

