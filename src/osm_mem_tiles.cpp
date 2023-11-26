#include "osm_mem_tiles.h"
using namespace std;

OsmMemTiles::OsmMemTiles(size_t threadNum, uint baseZoom, bool includeID)
	: TileDataSource(threadNum, baseZoom, includeID) 
{ }

void OsmMemTiles::Clear() {
	for (auto& entry : objects)
		entry.clear();
}
