#ifndef TILE_COORDINATES_SET_H
#define TILE_COORDINATES_SET_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>
#include "coordinates.h"
#ifdef _MSC_VER
#include <intrin.h>
#endif

// The index of the lowest set bit of v (v != 0)
inline unsigned lowestSetBit(uint64_t v) {
#if defined(__GNUC__) || defined(__clang__)
	return __builtin_ctzll(v);
#elif defined(_MSC_VER) && defined(_M_X64)
	unsigned long i;
	_BitScanForward64(&i, v);
	return i;
#else
	unsigned n = 0;
	while (!(v & 1)) { v >>= 1; n++; }
	return n;
#endif
}

// Interface representing a bitmap of tiles of interest at a given zoom.
class TileCoordinatesSet {
public:
	virtual bool test(TileCoordinate x, TileCoordinate y) const = 0;
	virtual void set(TileCoordinate x, TileCoordinate y) = 0;
	virtual size_t size() const = 0;
	virtual size_t zoom() const = 0;
	// Set every tile with minX <= x <= maxX and minY <= y <= maxY.
	virtual void setBox(TileCoordinate minX, TileCoordinate maxX, TileCoordinate minY, TileCoordinate maxY);
	// Visit every set tile in x-major, y-minor order.
	virtual void forEachSet(const std::function<void(TileCoordinate x, TileCoordinate y)>& f) const;
};

// Read-write implementation for precise sets; maximum zoom is
// generally expected to be z14.
class PreciseTileCoordinatesSet : public TileCoordinatesSet {
public:
	PreciseTileCoordinatesSet(unsigned int zoom);
	~PreciseTileCoordinatesSet();
	PreciseTileCoordinatesSet(const PreciseTileCoordinatesSet&) = delete;
	PreciseTileCoordinatesSet& operator=(const PreciseTileCoordinatesSet&) = delete;
	bool test(TileCoordinate x, TileCoordinate y) const override;
	size_t size() const override;
	size_t zoom() const override;
	void set(TileCoordinate x, TileCoordinate y) override;
	void setBox(TileCoordinate minX, TileCoordinate maxX, TileCoordinate minY, TileCoordinate maxY) override;
	void forEachSet(const std::function<void(TileCoordinate x, TileCoordinate y)>& f) const override;
	// Visit the set tiles with xBegin <= x < xEnd, x-major, y-minor
	template<typename F> void forEachSetInColumns(TileCoordinate xBegin, TileCoordinate xEnd, F f) const {
		const uint64_t first = (uint64_t)xBegin << zoom_, last = (uint64_t)xEnd << zoom_;	// bits [first, last)
		for (uint64_t w = first >> 6; w < numWords && (w << 6) < last; w++) {
			if (!((summary[w >> 12] >> ((w >> 6) & 63)) & 1)) { w |= 63; continue; }	// skip the empty block of 64 words
			uint64_t word = tiles[w];
			while (word) {
				const uint64_t loc = (w << 6) + lowestSetBit(word);
				word &= word - 1;
				if (loc < first || loc >= last) continue;
				f((TileCoordinate)(loc >> zoom_), (TileCoordinate)(loc & ((1ull << zoom_) - 1)));
			}
		}
	}
	// Add the tiles set in other, a set of the same zoom.
	void merge(const PreciseTileCoordinatesSet& other);
	// The same for the words under summary words [sBegin, sEnd) only: merges of disjoint ranges
	// can run at the same time.
	void mergeRange(const PreciseTileCoordinatesSet& other, size_t sBegin, size_t sEnd);
	size_t summaryWords() const { return summary.size(); }

private:
	unsigned int zoom_;
	uint64_t numBits;
	size_t numWords;
	uint64_t* tiles; // calloc'd, numWords words
	// 1 bit per block of 64 words of `tiles`; set when the block may hold a set bit.
	std::vector<uint64_t> summary;
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
	void forEachSet(const std::function<void(TileCoordinate x, TileCoordinate y)>& f) const override;

private:
	unsigned int zoom_;
	const TileCoordinatesSet& tiles;
	unsigned int scale;
};

#endif
