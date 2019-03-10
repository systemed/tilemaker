#include "osm_mem_tiles.h"
#include "osm_lua_processing.h"
using namespace std;

OsmMemTiles::OsmMemTiles(uint baseZoom,
		const vector<string> &inputFiles,
		const class Config &config,
		const std::string &luaFile,
		const class LayerDefinition &layers,	
		const class TileDataSource &shpMemTiles):

	TileDataSource(),
	baseZoom(baseZoom)
{
	class LayerDefinition layersTmp(layers);
	OsmLuaProcessing osmLuaProcessing(config, layersTmp, luaFile, 
		shpMemTiles, 
		*this);

	// ----	Read significant node tags
	vector<string> nodeKeyVec = osmLuaProcessing.GetSignificantNodeKeys();
	unordered_set<string> nodeKeys(nodeKeyVec.begin(), nodeKeyVec.end());

	// ----	Read all PBFs

	class PbfReader pbfReader;
	pbfReader.output = &osmLuaProcessing;
	for (auto inputFile : inputFiles) {

		cout << "Reading " << inputFile << endl;

		int ret = pbfReader.ReadPbfFile(inputFile, nodeKeys);
		if(ret != 0)
			cerr << "Error reading input " << inputFile << endl;
	}
}

void OsmMemTiles::GenerateTileListAtZoom(uint zoom, TileCoordinatesSet &dstCoords)
{
	::GenerateTileListFromTileIndex(zoom, baseZoom, tileIndex, dstCoords);
}

void OsmMemTiles::GetTileData(TileCoordinates dstIndex, uint zoom, 
	std::vector<OutputObjectRef> &dstTile)
{
	::GetTileDataFromTileIndex(dstIndex, zoom, baseZoom, tileIndex, dstTile);
}

void OsmMemTiles::AddObject(TileCoordinates index, OutputObjectRef oo)
{
	tileIndex[index].push_back(oo);
}

uint OsmMemTiles::GetBaseZoom()
{
	return baseZoom;
}

