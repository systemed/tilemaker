#ifndef TILE_COORDINATES_SET_H
#define TILE_COORDINATES_SET_H

#include <cstddef>
#include "coordinates.h"

// Interface representing a bitmap of tiles of interest at a given zoom.
class TileCoordinatesSet {
public:
	virtual bool test(TileCoordinate x, TileCoordinate y) const = 0;
	virtual void set(TileCoordinate x, TileCoordinate y) = 0;
	virtual size_t size() const = 0;
	virtual size_t zoom() const = 0;
};

// Read-write implementation for precise sets; maximum zoom is
// generally expected to be z14.
class PreciseTileCoordinatesSet : public TileCoordinatesSet {
public:
	PreciseTileCoordinatesSet(unsigned int zoom);
	bool test(TileCoordinate x, TileCoordinate y) const override;
	size_t size() const override;
	size_t zoom() const override;
	void set(TileCoordinate x, TileCoordinate y) override;

private:
	unsigned int zoom_;
	std::vector<bool> tiles;
};

// Read-only implementation for a lossy set. Used when zoom is
// z15 or higher, extrapolates a result based on a set for a lower zoom.
class LossyTileCoordinatesSet : public TileCoordinatesSet {
public:
	LossyTileCoordinatesSet(unsigned int zoom, const TileCoordinatesSet& precise);
	bool test(TileCoordinate x, TileCoordinate y) const override;
	size_t size() const override;
	size_t zoom() const override;
	void set(TileCoordinate x, TileCoordinate y) override;

private:
	unsigned int zoom_;
	const TileCoordinatesSet& tiles;
	unsigned int scale;
};

#endif
