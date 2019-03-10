/*! \file */ 
#ifndef _SHP_MEM_TILES
#define _SHP_MEM_TILES

#include "tile_data.h"
#include "shared_data.h"

class ShpMemTiles : public TileDataSource
{
public:
	ShpMemTiles(uint baseZoom);

	virtual void GenerateTileListAtZoom(uint zoom, TileCoordinatesSet &dstCoords);

	virtual void GetTileData(TileCoordinates dstIndex, uint zoom, 
		std::vector<OutputObjectRef> &dstTile);

	// Find intersecting shapefile layer
	virtual std::vector<std::string> FindIntersecting(const std::string &layerName, Box &box) const;
	virtual bool Intersects(const std::string &layerName, Box &box) const;

	virtual uint GetBaseZoom();

	virtual void Load(class LayerDefinition &layers, 
		bool hasClippingBox,
		const Box &clippingBox);

private:

	class TileIndexCached tileIndex;
};

#endif //_OSM_MEM_TILES

