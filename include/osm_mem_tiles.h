#ifndef _OSM_MEM_TILES
#define _OSM_MEM_TILES

#include "tile_data.h"
#include "osm_store.h"

class OsmMemTiles : public TileDataSource
{
public:
	OsmMemTiles(uint baseZoom);

	virtual void MergeTileCoordsAtZoom(uint zoom, TileCoordinatesSet &dstCoords);

	virtual void MergeSingleTileDataAtZoom(TileCoordinates dstIndex, uint zoom, 
		std::vector<OutputObjectRef> &dstTile);

	virtual void AddObject(TileCoordinates index, OutputObjectRef oo);

private:
	TileIndex tileIndex;
	uint baseZoom;
};

#endif //_OSM_MEM_TILES

