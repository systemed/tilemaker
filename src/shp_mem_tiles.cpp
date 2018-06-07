#include "shp_mem_tiles.h"
using namespace std;

ShpMemTiles::ShpMemTiles(uint baseZoom):
	TileDataSource(),
	baseZoom(baseZoom)
{

}

void ShpMemTiles::MergeTileCoordsAtZoom(uint zoom, TileCoordinatesSet &dstCoords)
{
	::MergeTileCoordsAtZoom(zoom, baseZoom, tileIndex, dstCoords);
}

void ShpMemTiles::MergeSingleTileDataAtZoom(TileCoordinates dstIndex, uint zoom, 
	std::vector<OutputObjectRef> &dstTile)
{
	::MergeSingleTileDataAtZoom(dstIndex, zoom, baseZoom, tileIndex, dstTile);
}

