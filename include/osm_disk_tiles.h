/*! \file */ 
#ifndef _OSM_DISK_TILES
#define _OSM_DISK_TILES

#include "tile_data.h"
#include "osm_store.h"

class OsmDiskTmpTiles : public TileDataSource
{
public:
	OsmDiskTmpTiles(uint baseZoom);

	virtual void MergeTileCoordsAtZoom(uint zoom, TileCoordinatesSet &dstCoords) {};

	virtual void MergeSingleTileDataAtZoom(TileCoordinates dstIndex, uint zoom, 
		std::vector<OutputObjectRef> &dstTile) {};

	virtual void AddObject(TileCoordinates index, OutputObjectRef oo);

	TileIndex tileIndex;
private:
	uint baseZoom;
};

/**
	\brief OsmDiskTiles reads OSM objects on disk and provides a vector of OutputObjectRef for specified tiles
	
	The data is read from a set of pbf files. The output objects are sent to OsmMemTiles for storage.
*/
class OsmDiskTiles : public TileDataSource
{
public:
	OsmDiskTiles(uint tilesZoom,
		const class Config &config,
		const std::string &luaFile,
		const class LayerDefinition &layers,	
		const class TileDataSource &shpData);

	virtual void MergeTileCoordsAtZoom(uint zoom, TileCoordinatesSet &dstCoords);

	virtual void MergeSingleTileDataAtZoom(TileCoordinates dstIndex, uint zoom, 
		std::vector<OutputObjectRef> &dstTile);

	virtual void AddObject(TileCoordinates index, OutputObjectRef oo);

private:
	const uint tilesZoom;
	const class Config &config;
	const std::string luaFile;
	const class LayerDefinition &layers;
	const class TileDataSource &shpData;

	bool tileBoundsSet;
	int xMin, xMax, yMin, yMax;
};

#endif //_OSM_DISK_TILES

