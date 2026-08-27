/*! \file */ 
#ifndef _DECLUTTER_H
#define _DECLUTTER_H

#include <mutex>
#include <vector>
#include "coordinates.h"
#include "output_object.h"

class TileDataSource;
struct LayerDef;

/// A point feature held back from the tile index until its minimum zoom has been decided
struct DeclutterEntry {
	OutputObject oo;
	LatpLon point;
	uint64_t id;
	int32_t score;
	bool fromShapefile;
};

/**
	\brief Thins out point features so only the most important ones appear at low zooms.

	Layers with declutter_below set don't index their point features as they're read: the
	features are parked here, together with the score their Lua profile gave them with
	Score(). Once everything has been read, apply() works up through the zoom levels,
	placing the highest-scoring features first and holding back any that fall too close to
	one already placed, then hands them all to the tile index.

	Ranking globally rather than per z6 tile means features either side of a z6 boundary
	still compete with each other.
*/
class Declutter {

public:
	/// Note which layers are decluttered - call before any add()
	void configure(const std::vector<LayerDef>& layers);

	bool inUse() const { return anyLayers; }
	bool isDecluttered(uint_least8_t layer) const { return anyLayers && layerEnabled[layer]; }

	/// Park a point feature (thread-safe)
	void add(const OutputObject& oo, LatpLon point, uint64_t id, int32_t score, bool fromShapefile);

	/// Assign minimum zooms, then write everything to the tile index
	void apply(const std::vector<LayerDef>& layers, TileDataSource& osmSource, TileDataSource& shpSource);

private:
	bool anyLayers = false;
	std::vector<bool> layerEnabled;
	std::vector<std::vector<DeclutterEntry>> entries;	// one per layer
	std::vector<std::mutex> mutexes;					//  |
};

#endif //_DECLUTTER_H
