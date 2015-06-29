// Write to MBTiles (sqlite) database
// (note that sqlite_modern_cpp.h is very slightly changed from the original, for blob support and an .init method)

class MBTiles { public:

	database db;

	MBTiles() {
	}

	void open(string *filename) {
		db.init(*filename);
		db << "CREATE TABLE IF NOT EXISTS metadata (name text, value text, UNIQUE (name));";
		db << "CREATE TABLE IF NOT EXISTS tiles (zoom_level integer, tile_column integer, tile_row integer, tile_data blob, UNIQUE (zoom_level, tile_column, tile_row));";
	}
	
	void writeMetadata(string key, string value) {
		db << "REPLACE INTO metadata (name,value) VALUES (?,?);" << key << value;
	}
	
	void saveTile(int zoom, int x, int y, string *data) {
		db << "REPLACE INTO tiles (zoom_level, tile_column, tile_row, tile_data) VALUES (?,?,?,?);" << zoom << x << y && *data;
	}
};
