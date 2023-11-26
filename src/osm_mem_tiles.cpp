#include "osm_mem_tiles.h"
using namespace std;

OsmMemTiles::OsmMemTiles(uint baseZoom)
	: TileDataSource(baseZoom) 
{ }

void OsmMemTiles::Clear() {
	for (auto& entry : objects)
		entry.clear();
}
