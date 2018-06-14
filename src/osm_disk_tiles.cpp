#include "osm_disk_tiles.h"
#include <boost/filesystem.hpp>
#include <iostream>
using namespace std;
using namespace boost::filesystem;

string PathTrailing(const path &in)
{
	string out;
	for (const path &pp : in)
		out = pp.string();
	return out;
}

OsmDiskTiles::OsmDiskTiles(uint baseZoom, uint tilesZoom):
	TileDataSource(),
	baseZoom(baseZoom),
	tilesZoom(tilesZoom)
{
	tileBoundsSet = false;
	xMin = 0; xMax = 0; yMin = 0; yMax = 0;

	// Determine extent of available tile files
	path p (to_string(tilesZoom));
	directory_iterator end_itr;
	bool firstDir = true;
	for (directory_iterator itr(p); itr != end_itr; ++itr)
	{
		if(!is_directory(itr->path()))
			continue;
		
		int x = std::stoi( PathTrailing(itr->path()) );
		if(!tileBoundsSet || x > xMax)
			xMax = x;
		if(!tileBoundsSet || x < xMin)
			xMin = x;

		if(firstDir)
		{
			path p2 (itr->path());
			for (directory_iterator itr2(p2); itr2 != end_itr; ++itr2)
			{
				if(!is_regular_file(itr2->path())) continue;
				int y = std::stoi( PathTrailing(itr2->path()) );
				if(!tileBoundsSet || y > yMax)
					yMax = y;
				if(!tileBoundsSet || y < yMin)
					yMin = y;		
				tileBoundsSet = true;
			}
		}
		
		firstDir = false;
	}
	cout << "x " << xMin << "," << xMax << endl;
	cout << "y " << yMin << "," << yMax << endl;
}

void OsmDiskTiles::MergeTileCoordsAtZoom(uint zoom, TileCoordinatesSet &dstCoords)
{
	//::MergeTileCoordsAtZoom(zoom, baseZoom, tileIndex, dstCoords);
}

void OsmDiskTiles::MergeSingleTileDataAtZoom(TileCoordinates dstIndex, uint zoom, 
	std::vector<OutputObjectRef> &dstTile)
{
	//::MergeSingleTileDataAtZoom(dstIndex, zoom, baseZoom, tileIndex, dstTile);
}

void OsmDiskTiles::AddObject(TileCoordinates index, OutputObjectRef oo)
{
	//This is called when loading pbfs during initialization. OsmDiskTiles don't need that
	//info, so do nothing.
}
