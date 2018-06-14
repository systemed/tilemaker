#include "osm_disk_tiles.h"
using namespace std;

OsmDiskTiles::OsmDiskTiles(uint baseZoom):
	TileDataSource(),
	baseZoom(baseZoom)
{

}

void OsmDiskTiles::MergeTileCoordsAtZoom(uint zoom, TileCoordinatesSet &dstCoords)
{
	::MergeTileCoordsAtZoom(zoom, baseZoom, tileIndex, dstCoords);
}

void OsmDiskTiles::MergeSingleTileDataAtZoom(TileCoordinates dstIndex, uint zoom, 
	std::vector<OutputObjectRef> &dstTile)
{
	::MergeSingleTileDataAtZoom(dstIndex, zoom, baseZoom, tileIndex, dstTile);
}

void OsmDiskTiles::AddObject(TileCoordinates index, OutputObjectRef oo)
{
	//This is called when loading pbfs during initialization. OsmDiskTiles don't need that
	//info, so do nothing.
}
