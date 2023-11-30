
#include "mbtiles.h"
#include "helpers.h"
#include <cmath>
#include <boost/iostreams/filtering_streambuf.hpp>
#include <boost/iostreams/copy.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/device/array.hpp>

using namespace sqlite;
using namespace std;
namespace bio = boost::iostreams;

MBTiles::MBTiles() {}

MBTiles::~MBTiles() {
	if (db && inTransaction) db << "COMMIT;"; // commit all the changes if open
}

// ---- Write .mbtiles

void MBTiles::openForWriting(string *filename) {
	db.init(*filename);
	db << "PRAGMA synchronous = OFF;";
	try {
		db << "PRAGMA application_id = 0x4d504258;";
	} catch(runtime_error &e) {
		cout << "Couldn't write SQLite application_id (not fatal): " << e.what() << endl;
	}
	try {
		db << "PRAGMA journal_mode=OFF;";
	} catch(runtime_error &e) {
		cout << "Couldn't turn journaling off (not fatal): " << e.what() << endl;
	}
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
	
void MBTiles::saveTile(int zoom, int x, int y, string *data, bool isMerge) {
	int tmsY = pow(2,zoom) - 1 - y;
	m.lock();
	if (isMerge) {
		db << "REPLACE INTO tiles (zoom_level, tile_column, tile_row, tile_data) VALUES (?,?,?,?)" << zoom << x << tmsY && *data;
	} else {
		db << "INSERT INTO tiles (zoom_level, tile_column, tile_row, tile_data) VALUES (?,?,?,?)" << zoom << x << tmsY && *data;
	}
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

bool MBTiles::readTileAndUncompress(string &data, int zoom, int x, int y, bool isCompressed, bool asGzip) {
	m.lock();
	int tmsY = pow(2,zoom) - 1 - y;
	int exists=0;
	db << "SELECT COUNT(*) FROM tiles WHERE zoom_level=? AND tile_column=? AND tile_row=?" << zoom << x << tmsY >> exists;
	m.unlock();
	if (exists==0) return false;

	m.lock();
	std::vector<char> compressed;
	db << "SELECT tile_data FROM tiles WHERE zoom_level=? AND tile_column=? AND tile_row=?" << zoom << x << tmsY >> compressed;
	m.unlock();
	try {
		bio::stream<bio::array_source> in(compressed.data(), compressed.size());
		bio::filtering_streambuf<bio::input> out;

		if (isCompressed) {
			if (asGzip) { out.push(bio::gzip_decompressor()); }
			else { out.push(bio::zlib_decompressor()); }
		}
		out.push(in);

		std::stringstream decompressed;
		bio::copy(out, decompressed);
		data = decompressed.str();
		return true;
	} catch(std::runtime_error &e) {
		return false;
	}
}
