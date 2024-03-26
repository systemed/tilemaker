#include "tile_coordinates_set.h"

PreciseTileCoordinatesSet::PreciseTileCoordinatesSet(uint zoom):
	zoom_(zoom),
	tiles((1 << zoom) * (1 << zoom)) {}

bool PreciseTileCoordinatesSet::test(TileCoordinate x, TileCoordinate y) const {
	uint64_t loc = x * (1 << zoom_) + y;
	if (loc >= tiles.size())
		return false;

	return tiles[loc];
}

size_t PreciseTileCoordinatesSet::zoom() const {
	return zoom_;
}

size_t PreciseTileCoordinatesSet::size() const {
	size_t rv = 0;
	for (int i = 0; i < tiles.size(); i++)
		if (tiles[i])
			rv++;

	return rv;
}

void PreciseTileCoordinatesSet::set(TileCoordinate x, TileCoordinate y) {
	uint64_t loc = x * (1 << zoom_) + y;
	if (loc >= tiles.size())
		return;
	tiles[loc] = true;
}

LossyTileCoordinatesSet::LossyTileCoordinatesSet(uint zoom, const TileCoordinatesSet& underlying) : zoom_(zoom), tiles(underlying), scale(1 << (zoom - underlying.zoom())) {
	if (zoom <= underlying.zoom())
		throw std::out_of_range("LossyTileCoordinatesSet: zoom (" + std::to_string(zoom_) + ") must be greater than underlying set's zoom (" + std::to_string(underlying.zoom()) + ")");
}

bool LossyTileCoordinatesSet::test(TileCoordinate x, TileCoordinate y) const {
	return tiles.test(x / scale, y / scale);
}

size_t LossyTileCoordinatesSet::size() const {
	return tiles.size() * scale * scale;
}

size_t LossyTileCoordinatesSet::zoom() const {
	return zoom_;
}

void LossyTileCoordinatesSet::set(TileCoordinate x, TileCoordinate y) {
	throw std::runtime_error("LossyTileCoordinatesSet::set() is not implemented; LossyTileCoordinatesSet is read-only");
}

