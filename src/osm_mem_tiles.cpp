#include "osm_mem_tiles.h"
using namespace std;

OsmMemTiles::OsmMemTiles(size_t threadNum, uint baseZoom)
	: TileDataSource(threadNum, baseZoom) 
{ }

void OsmMemTiles::Clear() {
	for (auto& entry : objects)
		entry.clear();
}
