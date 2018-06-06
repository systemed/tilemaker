#include "tile_data.h"

TileData::TileData(const OSMStore &osmStore, const std::vector<Geometry> &cachedGeometries):
	osmStore(osmStore),
	cachedGeometries(cachedGeometries)
{
	this->tileIndexForZoom = nullptr;
}

