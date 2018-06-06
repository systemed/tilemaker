#include <algorithm>
#include "tile_data.h"
using namespace std;

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

TileData::TileData()
{
	this->tileIndexForZoom = nullptr;
}

class TilesAtZoomIterator TileData::GetTilesAtZoomBegin()
{
	return TilesAtZoomIterator(tileIndexForZoom->begin(), *this);
}

class TilesAtZoomIterator TileData::GetTilesAtZoomEnd()
{
	return TilesAtZoomIterator(tileIndexForZoom->end(), *this);
}

size_t TileData::GetTilesAtZoomSize()
{
	return tileIndexForZoom->size();
}

void TileData::SetTileIndexForZoom(const TileIndex *tileIndexForZoom)
{
	this->tileIndexForZoom = tileIndexForZoom;
}

