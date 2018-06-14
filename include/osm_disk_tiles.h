/*! \file */ 
#ifndef _OSM_DISK_TILES
#define _OSM_DISK_TILES

#include "tile_data.h"
#include "osm_store.h"

/**
	\brief OsmDiskTiles reads OSM objects on disk and provides a vector of OutputObjectRef for specified tiles
	
	The data is read from a set of pbf files. The output objects are sent to OsmMemTiles for storage.
*/
class OsmDiskTiles : public TileDataSource
{
public:
	OsmDiskTiles(uint baseZoom);

	virtual void MergeTileCoordsAtZoom(uint zoom, TileCoordinatesSet &dstCoords);

	virtual void MergeSingleTileDataAtZoom(TileCoordinates dstIndex, uint zoom, 
		std::vector<OutputObjectRef> &dstTile);

	virtual void AddObject(TileCoordinates index, OutputObjectRef oo);

private:
	TileIndex tileIndex;
	uint baseZoom;
};

#endif //_OSM_DISK_TILES

