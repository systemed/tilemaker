#ifndef _TILE_DATA_H
#define _TILE_DATA_H

#include <map>
#include "osm_store.h"
#include "output_object.h"

class TileData
{
public:
	TileData(const OSMStore &osmStore, const std::vector<Geometry> &cachedGeometries);

	//The plan is to make this class a facade, and somehow hide the detail of these variables
	const OSMStore &osmStore;
	const std::map<TileCoordinates, std::vector<OutputObject>, TileCoordinatesCompare > *tileIndexForZoom;
	const std::vector<Geometry> &cachedGeometries;
};

#endif //_TILE_DATA_H
