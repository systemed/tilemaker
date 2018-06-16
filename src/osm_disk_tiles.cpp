#include "osm_disk_tiles.h"
#include "osm_lua_processing.h"
#include <boost/filesystem.hpp>
#include <iostream>
using namespace std;
using namespace boost::filesystem;

OsmDiskTmpTiles::OsmDiskTmpTiles(uint baseZoom):
	TileDataSource(),
	baseZoom(baseZoom)
{

}

void OsmDiskTmpTiles::AddObject(TileCoordinates index, OutputObjectRef oo)
{
	tileIndex[index].push_back(oo);
}

// ********************************************

string PathTrailing(const path &in)
{
	string out;
	for (const path &pp : in)
		out = pp.string();
	return out;
}

OsmDiskTiles::OsmDiskTiles(uint tilesZoom,
		const class Config &config,
		const std::string &luaFile,
		const class LayerDefinition &layers,	
		const class TileDataSource &shpData):
	TileDataSource(),
	tilesZoom(tilesZoom),
	config(config),
	luaFile(luaFile),
	layers(layers),
	shpData(shpData)
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
	if (zoom==tilesZoom) {
		// at native zoom level
		for (int x=xMin; x<=xMax; x++)
			for (int y=yMin; y<=yMax; y++)
				dstCoords.insert(TileCoordinates(x, y));
	} else {
		// otherwise, we need to run through the native zoom list, and assign each way
		// to a tile at our zoom level
		if(zoom < tilesZoom)
		{
			int scale = pow(2, tilesZoom-zoom);
			for (int x=xMin; x<=xMax; x++)
			{
				TileCoordinate tilex = x / scale;
				for (int y=yMin; y<=yMax; y++)
				{			
					TileCoordinate tiley = y / scale;
					TileCoordinates newIndex(tilex, tiley);
					dstCoords.insert(newIndex);
				}
			}
		}
		else
		{
			int scale = pow(2, zoom-tilesZoom);
			TileCoordinate xMinScaled = xMin * scale;
			TileCoordinate xMaxScaled = (xMax+1) * scale;
			TileCoordinate yMinScaled = yMin * scale;
			TileCoordinate yMaxScaled = (yMax+1) * scale;
			
			for (int x=xMinScaled; x<xMaxScaled; x++)
			{
				for (int y=yMinScaled; y<yMaxScaled; y++)
				{			
					TileCoordinates newIndex(x, y);
					dstCoords.insert(newIndex);
				}
			}
		}

	}
}

void OsmDiskTiles::MergeSingleTileDataAtZoom(TileCoordinates dstIndex, uint zoom, 
	std::vector<OutputObjectRef> &dstTile)
{
	if(zoom < tilesZoom)
	{

	}
	else
	{
		//Convert request tile coordinates into the source tile used for input
		TileCoordinate tilex = dstIndex.x;
		TileCoordinate tiley = dstIndex.y;
		if(zoom > tilesZoom)
		{
			int scale = pow(2, zoom-tilesZoom);
			tilex = dstIndex.x / scale;
			tiley = dstIndex.y / scale;
		}

		class LayerDefinition layersTmp(layers);
		class OsmDiskTmpTiles tmpTiles(config.baseZoom);

		OsmLuaProcessing osmLuaProcessing(config, layersTmp, luaFile, 
			shpData, 
			tmpTiles);

		// ----	Read significant node tags
		vector<string> nodeKeyVec = osmLuaProcessing.GetSignificantNodeKeys();
		unordered_set<string> nodeKeys(nodeKeyVec.begin(), nodeKeyVec.end());

		// ----	Read PBF file
	
		class PbfReader pbfReader;
		pbfReader.output = &osmLuaProcessing;

		path inputFile(to_string(tilesZoom));
		inputFile /= to_string(tilex); 
		inputFile /= to_string(tiley) + ".pbf";
		cout << inputFile << endl;
		int ret = pbfReader.ReadPbfFile(inputFile.string(), nodeKeys);
		if(ret != 0)
			return;

		::MergeSingleTileDataAtZoom(dstIndex, zoom, config.baseZoom, tmpTiles.tileIndex, dstTile);
	}

}

void OsmDiskTiles::AddObject(TileCoordinates index, OutputObjectRef oo)
{
	//This is called when loading pbfs during initialization. OsmDiskTiles don't need that
	//info, so do nothing.
}
