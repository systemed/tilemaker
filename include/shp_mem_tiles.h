#ifndef _SHP_MEM_TILES
#define _SHP_MEM_TILES

#include "tile_data.h"

class ShpMemTiles : public TileDataSource
{
public:
	ShpMemTiles(uint baseZoom);

	virtual void MergeTileCoordsAtZoom(uint zoom, TileCoordinatesSet &dstCoords);

	virtual void MergeSingleTileDataAtZoom(TileCoordinates dstIndex, uint zoom, 
		std::vector<OutputObjectRef> &dstTile);

	TileIndex tileIndex;
	std::vector<Geometry> cachedGeometries;					// prepared boost::geometry objects (from shapefiles)
	std::map<uint, std::string> cachedGeometryNames;			//  | optional names for each one

private:
	uint baseZoom;
};

#endif //_OSM_MEM_TILES

