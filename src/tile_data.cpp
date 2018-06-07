#include <algorithm>
#include "tile_data.h"
using namespace std;

typedef std::pair<OutputObjectsConstIt,OutputObjectsConstIt> OutputObjectsConstItPair;

void MergeTileDataAtZoom(uint zoom, uint baseZoom, const TileIndex &srcTiles, TileIndex &dstTiles)
{
	if (zoom==baseZoom) {
		// at z14, we can just use tileIndex
		for (auto it = srcTiles.begin(); it!= srcTiles.end(); ++it) {
			TileCoordinates index = it->first;
			dstTiles[index].insert(dstTiles[index].end(), it->second.begin(), it->second.end());
		}
	} else {
		// otherwise, we need to run through the z14 list, and assign each way
		// to a tile at our zoom level
		for (auto it = srcTiles.begin(); it!= srcTiles.end(); ++it) {
			TileCoordinates index = it->first;
			TileCoordinate tilex = index.x / pow(2, baseZoom-zoom);
			TileCoordinate tiley = index.y / pow(2, baseZoom-zoom);
			TileCoordinates newIndex(tilex, tiley);
			const vector<OutputObjectRef> &ooset = it->second;
			for (auto jt = ooset.begin(); jt != ooset.end(); ++jt) {
				dstTiles[newIndex].push_back(*jt);
			}
		}
	}
}

// *********************************

ObjectsAtSubLayerIterator::ObjectsAtSubLayerIterator(OutputObjectsConstIt it, const class TileData &tileData):
	tileData(tileData)
{
	*(OutputObjectsConstIt *)this = it;
}

// ********************************

TilesAtZoomIterator::TilesAtZoomIterator(TileIndex::const_iterator it, class TileData &tileData):
	tileData(tileData)
{
	*(TileIndex::const_iterator *)this = it;
}

TileCoordinates TilesAtZoomIterator::GetCoordinates() const
{
	TileIndex::const_iterator it = *this;
	return it->first;
}

ObjectsAtSubLayerConstItPair TilesAtZoomIterator::GetObjectsAtSubLayer(uint_least8_t layerNum) const
{
	// compare only by `layer`
	auto layerComp = [](const OutputObjectRef &x, const OutputObjectRef &y) -> bool { return x->layer < y->layer; };
	// We get the range within ooList, where the layer of each object is `layerNum`.
	// Note that ooList is sorted by a lexicographic order, `layer` being the most significant.
	TileIndex::const_iterator it = *this;
	const std::vector<OutputObjectRef> &ooList = it->second;
	OutputObjectRef referenceObj = make_shared<OutputObjectOsmStore>(POINT, layerNum, 0, *(OSMStore *)nullptr);
	OutputObjectsConstItPair ooListSameLayer = equal_range(ooList.begin(), ooList.end(), referenceObj, layerComp);
	return ObjectsAtSubLayerConstItPair(ObjectsAtSubLayerIterator(ooListSameLayer.first, tileData), ObjectsAtSubLayerIterator(ooListSameLayer.second, tileData));
}

// *********************************

TileData::TileData(const TileIndex &tileIndexPbf, const TileIndex &tileIndexShp, uint baseZoom):
	tileIndexPbf(tileIndexPbf),
	tileIndexShp(tileIndexShp),
	baseZoom(baseZoom)
{

}

class TilesAtZoomIterator TileData::GetTilesAtZoomBegin()
{
	return TilesAtZoomIterator(generatedIndex.begin(), *this);
}

class TilesAtZoomIterator TileData::GetTilesAtZoomEnd()
{
	return TilesAtZoomIterator(generatedIndex.end(), *this);
}

size_t TileData::GetTilesAtZoomSize()
{
	return generatedIndex.size();
}

void TileData::SetZoom(uint zoom)
{
	// Create list of tiles, and the data in them
	generatedIndex.clear();
	MergeTileDataAtZoom(zoom, baseZoom, tileIndexPbf, generatedIndex);
	MergeTileDataAtZoom(zoom, baseZoom, tileIndexShp, generatedIndex);

	// ----	Sort each tile
	for (auto it = generatedIndex.begin(); it != generatedIndex.end(); ++it) {
		auto &ooset = it->second;
		sort(ooset.begin(), ooset.end());
		ooset.erase(unique(ooset.begin(), ooset.end()), ooset.end());
	}
}

