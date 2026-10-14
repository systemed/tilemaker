#include "tile_coordinates_set.h"
#include <string>
#include <stdexcept>
#include <algorithm>
#include <cstdlib>
#include <new>
#ifdef TMOPT_VERIFY
#include <cstdio>
#endif
#ifdef _MSC_VER
#include <intrin.h>
#endif

static inline unsigned int countTrailingZeros(uint64_t v) {
#if defined(__GNUC__) || defined(__clang__)
	return __builtin_ctzll(v);
#elif defined(_MSC_VER) && defined(_M_X64)
	unsigned long idx;
	_BitScanForward64(&idx, v);
	return idx;
#else
	unsigned int n = 0;
	while (!(v & 1)) { v >>= 1; n++; }
	return n;
#endif
}

static inline unsigned int popCount(uint64_t v) {
#if defined(__GNUC__) || defined(__clang__)
	return __builtin_popcountll(v);
#else
	unsigned int n = 0;
	while (v) { v &= v - 1; n++; }
	return n;
#endif
}

void TileCoordinatesSet::forEachSet(const std::function<void(TileCoordinate x, TileCoordinate y)>& f) const {
	const TileCoordinate dim = 1u << zoom();
	for (TileCoordinate x = 0; x < dim; x++)
		for (TileCoordinate y = 0; y < dim; y++)
			if (test(x, y))
				f(x, y);
}

PreciseTileCoordinatesSet::PreciseTileCoordinatesSet(unsigned int zoom):
	zoom_(zoom),
	numBits((uint64_t)(1 << zoom) * (1 << zoom)),
	numWords((numBits + 63) / 64),
	tiles(nullptr),
	summary(((numWords + 63) / 64 + 63) / 64) {
	// calloc: untouched pages stay unmapped (a z14 set is 32 MB).
	tiles = static_cast<uint64_t*>(std::calloc(numWords, sizeof(uint64_t)));
	if (tiles == nullptr)
		throw std::bad_alloc();
}

PreciseTileCoordinatesSet::~PreciseTileCoordinatesSet() {
	std::free(tiles);
}

bool PreciseTileCoordinatesSet::test(TileCoordinate x, TileCoordinate y) const {
	uint64_t loc = x * (1 << zoom_) + y;
	if (loc >= numBits)
		return false;

	return (tiles[loc >> 6] >> (loc & 63)) & 1;
}

size_t PreciseTileCoordinatesSet::zoom() const {
	return zoom_;
}

// Call f(w) for each word index w in a block marked in the summary, ascending.
template<typename F>
static inline void forEachMarkedWord(const std::vector<uint64_t>& summary, size_t numWords, F f) {
	for (size_t s = 0; s < summary.size(); s++) {
		uint64_t bits = summary[s];
		while (bits) {
			const size_t block = s * 64 + countTrailingZeros(bits);
			bits &= bits - 1;
			const size_t wEnd = std::min((block + 1) * 64, numWords);
			for (size_t w = block * 64; w < wEnd; w++)
				f(w);
		}
	}
}

size_t PreciseTileCoordinatesSet::size() const {
	size_t rv = 0;
	forEachMarkedWord(summary, numWords, [&](size_t w) { rv += popCount(tiles[w]); });
	return rv;
}

void PreciseTileCoordinatesSet::set(TileCoordinate x, TileCoordinate y) {
	uint64_t loc = x * (1 << zoom_) + y;
	if (loc >= numBits)
		return;
	const uint64_t w = loc >> 6;
	tiles[w] |= (uint64_t)1 << (loc & 63);
	// 1 summary bit per block of 64 words
	summary[w >> 12] |= (uint64_t)1 << ((w >> 6) & 63);
}

void PreciseTileCoordinatesSet::forEachSet(const std::function<void(TileCoordinate x, TileCoordinate y)>& f) const {
	// loc = x * 2^zoom + y, so ascending loc order is x-major, y-minor.
	// Only blocks marked in the summary can hold set bits; visiting them in
	// ascending order gives the same sequence as a full scan.
#ifdef TMOPT_VERIFY
	{
		std::vector<uint64_t> full, fast;
		for (size_t w = 0; w < numWords; w++) {
			uint64_t word = tiles[w];
			while (word) {
				full.push_back((uint64_t)w * 64 + countTrailingZeros(word));
				word &= word - 1;
			}
		}
		forEachMarkedWord(summary, numWords, [&](size_t w) {
			uint64_t word = tiles[w];
			while (word) {
				fast.push_back((uint64_t)w * 64 + countTrailingZeros(word));
				word &= word - 1;
			}
		});
		if (full != fast || full.size() != size()) {
			std::fprintf(stderr, "TMOPT_VERIFY: PreciseTileCoordinatesSet z%u mismatch %zu vs %zu\n", zoom_, full.size(), fast.size());
			std::abort();
		}
	}
#endif
	forEachMarkedWord(summary, numWords, [&](size_t w) {
		uint64_t word = tiles[w];
		while (word) {
			uint64_t loc = (uint64_t)w * 64 + countTrailingZeros(word);
			word &= word - 1;
			f((TileCoordinate)(loc >> zoom_), (TileCoordinate)(loc & ((1ull << zoom_) - 1)));
		}
	});
}

LossyTileCoordinatesSet::LossyTileCoordinatesSet(unsigned int zoom, const TileCoordinatesSet& underlying) : zoom_(zoom), tiles(underlying), scale(1 << (zoom - underlying.zoom())) {
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
	throw std::logic_error("LossyTileCoordinatesSet::set() is not implemented; LossyTileCoordinatesSet is read-only");
}

