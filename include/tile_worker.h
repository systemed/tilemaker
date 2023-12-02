/*! \file */ 
#ifndef _TILE_WORKER_H
#define _TILE_WORKER_H

#include "tile_data.h"
#include "shared_data.h"

/// Start function for worker threads
void outputProc(
	SharedData& sharedData,
	const SourceList& sources,
	const AttributeStore& attributeStore,
	const std::vector<std::vector<OutputObjectID>>& data,
	TileCoordinates coordinates,
	uint zoom
);

#endif //_TILE_WORKER_H
