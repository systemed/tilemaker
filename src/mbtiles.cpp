
#include "mbtiles.h"
#include "helpers.h"
#include <cmath>
using namespace sqlite;
using namespace std;

MBTiles::MBTiles() {}

MBTiles::~MBTiles() {
	if (db && inTransaction) db << "COMMIT;"; // commit all the changes if open
}

// ---- Write .mbtiles

void MBTiles::openForWriting(string *filename) {
	db.init(*filename);
	db << "PRAGMA synchronous = OFF;";
	db << "PRAGMA application_id = 0x4d504258;";
	db << "CREATE TABLE IF NOT EXISTS metadata (name text, value text, UNIQUE (name));";
	db << "CREATE TABLE IF NOT EXISTS tiles (zoom_level integer, tile_column integer, tile_row integer, tile_data blob, UNIQUE (zoom_level, tile_column, tile_row));";
	db << "BEGIN;"; // begin a transaction
	inTransaction = true;
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

void MBTiles::closeForWriting() {
	db << "CREATE UNIQUE INDEX IF NOT EXISTS tile_index on tiles (zoom_level, tile_column, tile_row);";
}

// ---- Read mbtiles

void MBTiles::openForReading(string *filename) {
	db.init(*filename);
}

void MBTiles::readBoundingBox(double &minLon, double &maxLon, double &minLat, double &maxLat) {
	string boundsStr;
	db << "SELECT value FROM metadata WHERE name='bounds'" >> boundsStr;
	vector<string> b = split_string(boundsStr,',');
	minLon = stod(b[0]); minLat = stod(b[1]);
	maxLon = stod(b[2]); maxLat = stod(b[3]);
}

void MBTiles::readTileList(std::vector<std::tuple<int,int,int>> &tileList) {
	db << "SELECT zoom_level,tile_column,tile_row FROM tiles" >> [&](int z,int col, int row) {
		tileList.emplace_back(std::make_tuple(z,col,row));
	};
}

vector<char> MBTiles::readTile(int zoom, int col, int row) {
	vector<char> pbfBlob;
	db << "SELECT tile_data FROM tiles WHERE zoom_level=? AND tile_column=? AND tile_row=?" << zoom << col << row >> pbfBlob;
	return pbfBlob;
}
