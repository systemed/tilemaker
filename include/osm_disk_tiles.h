/*! \file */ 
#ifndef _OSM_DISK_TILES
#define _OSM_DISK_TILES

#include "tile_data.h"
#include "osm_store.h"

bool CheckAvailableDiskTileExtent(const std::string &basePath,
	Box &clippingBox);

/**
 * \brief Used by OsmDiskTiles has temporary storage while processing one or more tiles in a lazy fashion.
 */
class OsmDiskTmpTiles : public TileDataSource
{
public:
	OsmDiskTmpTiles(uint baseZoom);

	virtual void MergeTileCoordsAtZoom(uint zoom, TileCoordinatesSet &dstCoords) {};

	virtual void MergeSingleTileDataAtZoom(TileCoordinates dstIndex, uint zoom, 
		std::vector<OutputObjectRef> &dstTile) {};

	virtual void AddObject(TileCoordinates index, OutputObjectRef oo);

	virtual uint GetBaseZoom();

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
	OsmDiskTiles(const std::string &basePath,
		const class Config &config,
		const std::string &luaFile,
		const class LayerDefinition &layers,	
		const class TileDataSource &shpData);

	///This must be thread safe!
	virtual void MergeTileCoordsAtZoom(uint zoom, TileCoordinatesSet &dstCoords);

	///This must be thread safe!
	virtual void MergeSingleTileDataAtZoom(TileCoordinates dstIndex, uint zoom, 
		std::vector<OutputObjectRef> &dstTile);

	virtual void AddObject(TileCoordinates index, OutputObjectRef oo);

	virtual uint GetBaseZoom();

private:
	//This variables are generally safe for multiple threads to read, but not to write. (They are const anyway.)

	uint tilesZoom;
	const class Config &config;
	const std::string luaFile;
	const class LayerDefinition &layers;
	const class TileDataSource &shpData;

	bool tileBoundsSet;
	int xMin, xMax, yMin, yMax;
};

#endif //_OSM_DISK_TILES

