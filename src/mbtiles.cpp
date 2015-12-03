// Write to MBTiles (sqlite) database
// (note that sqlite_modern_cpp.h is very slightly changed from the original, for blob support and an .init method)

class MBTiles { public:

	database db;

	MBTiles() {
	}

	~MBTiles() {
		if (db) db << "COMMIT;"; // commit all the changes if open
	}

	void open(string *filename) {
		db.init(*filename);
		db << "PRAGMA synchronous = OFF;";
		db << "CREATE TABLE IF NOT EXISTS metadata (name text, value text, UNIQUE (name));";
		db << "CREATE TABLE IF NOT EXISTS tiles (zoom_level integer, tile_column integer, tile_row integer, tile_data blob, UNIQUE (zoom_level, tile_column, tile_row));";
		db << "BEGIN;"; // begin a transaction
	}
	
	void writeMetadata(string key, string value) {
		db << "REPLACE INTO metadata (name,value) VALUES (?,?);" << key << value;
	}
	
	void saveTile(int zoom, int x, int y, string *data) {
		int tmsY = pow(2,zoom) - 1 - y;
		db << "REPLACE INTO tiles (zoom_level, tile_column, tile_row, tile_data) VALUES (?,?,?,?);" << zoom << x << tmsY && *data;
	}
};
