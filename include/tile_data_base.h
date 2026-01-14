/*! \file */
#ifndef _TILE_DATA_BASE_H
#define _TILE_DATA_BASE_H

#include <cstdint>
#include "output_object.h"

#define TILE_DATA_ID_SIZE 34

// We cluster output objects by z6 tile
#define CLUSTER_ZOOM 6
#define CLUSTER_ZOOM_WIDTH (1 << CLUSTER_ZOOM)
#define CLUSTER_ZOOM_AREA (CLUSTER_ZOOM_WIDTH * CLUSTER_ZOOM_WIDTH)

// TileDataSource indexes which tiles have objects in them. The indexed zoom
// is at most z14; we'll clamp to z14 if the base zoom is higher than z14.
//
// As a result, we need at most 15 bits to store an X/Y coordinate. For efficiency,
// we bucket the world into 4,096 z6 tiles, which each contain some number of
// z14 objects. This lets us use only 8 bits to store an X/Y coordinate.
//
// Because index zoom is lower than base zoom in the case where base zoom is
// z15+, we'll get false positives when looking up objects in the index,
// since, e.g., a single z14 tile covers 4 z15 tiles.
//
// This is OK: when writing the z15 tile, there's a clipping step that will filter
// out the false positives.
typedef uint8_t Z6Offset;

struct OutputObjectXY {
	OutputObject oo;
	Z6Offset x;
	Z6Offset y;
};

struct OutputObjectXYID {
	OutputObject oo;
	Z6Offset x;
	Z6Offset y;
	uint64_t id;
};

#endif //_TILE_DATA_BASE_H
