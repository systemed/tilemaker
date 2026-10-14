#ifndef _CLIP_CACHE_H
#define _CLIP_CACHE_H

#include "coordinates.h"
#include "coordinates_geom.h"
#include "geom.h"
#include <mutex>
#include <unordered_map>
#ifdef TMOPT_VERIFY
#include <iostream>
#include <map>
#endif

class TileBbox;

// Identifies the tile batch the current thread is working on. The clip cache
// is per-thread and is reset whenever the batch changes, so a tile can only
// reuse clips of its ancestors from the same batch. This makes output depend
// only on the batch partition, not on thread count or timing.
extern thread_local uint64_t clipCacheBatchId;
// Set when the current batch can't reuse clips (a single tile), so add() is skipped.
extern thread_local bool clipCacheNoStore;

template <class T>
class ClipCache {
public:
	ClipCache(size_t threadNum, unsigned int baseZoom):
		baseZoom(baseZoom) {
	}

	// An entry holds the clipped geometry and whether it is known to be valid
	// (passed geom::is_valid). Callers that don't track validity get false.
	struct Entry {
		std::shared_ptr<T> geom;
		bool valid = false;
	};

	const std::shared_ptr<T> get(uint zoom, TileCoordinate x, TileCoordinate y, NodeID objectID) const {
		return getEntry(zoom, x, y, objectID).geom;
	}

	Entry getEntry(uint zoom, TileCoordinate x, TileCoordinate y, NodeID objectID) const {
		// Look for a previously clipped version at z-1, z-2, ... (nearest ancestor first)
		LocalStore& s = local(zoom, TileCoordinates(x, y));
		Entry rv;
		const Key key{(const void*)this, objectID};
		for (uint z = zoom < MAX_LEVELS ? zoom : MAX_LEVELS; z-- > 0; ) {
			if (!(s.usedMask & (1u << z)))
				continue;
			const Level& level = s.levels[z];
			const uint shift = zoom - z;
			if (level.index.x != ((uint64_t)x >> shift) || level.index.y != ((uint64_t)y >> shift))
				continue;
			const auto it = level.m.find(key);
			if (it != level.m.end()) {
				rv = it->second;
				break;
			}
		}
#ifdef TMOPT_VERIFY
		{
			Entry old;
			uint vz = zoom;
			TileCoordinate vx = x, vy = y;
			while (vz > 0) {
				vz--;
				vx /= 2;
				vy /= 2;
				const auto& it = s.shadow.find(std::make_tuple((uint16_t)vz, (const void*)this, TileCoordinates(vx, vy), objectID));
				if (it != s.shadow.end()) {
					old = it->second;
					break;
				}
			}
			if (old.geom != rv.geom || old.valid != rv.valid) {
				std::cerr << "TMOPT_VERIFY: ClipCache mismatch z" << zoom << " " << x << "/" << y << " id " << objectID << std::endl;
				abort();
			}
		}
#endif
		return rv;
	}

	void add(const TileBbox& bbox, const NodeID objectID, const T& output, bool valid = false) {
		// The point of caching is to reuse the clip, so caching at the terminal zoom is
		// pointless.
		if (bbox.zoom == baseZoom || clipCacheNoStore)
			return;

		// Levels beyond MAX_LEVELS could never be looked up by a child tile anyway
		// (tile zooms are far below this).
		if (bbox.zoom >= MAX_LEVELS)
			return;

		std::shared_ptr<T> copy = std::make_shared<T>();
		boost::geometry::assign(*copy, output);

		LocalStore& s = local(bbox.zoom, bbox.index);
		// Safety cap (pruning normally keeps the store small). Count-based, so
		// the result stays deterministic.
		if (s.total > (1 << 18))
			s.clearFrom(0);
#ifdef TMOPT_VERIFY
		if (s.shadow.size() > (1 << 18))
			s.shadow.clear();
		s.shadow[std::make_tuple((uint16_t)bbox.zoom, (const void*)this, bbox.index, objectID)] = Entry{copy, valid};
#endif

		Level& level = s.levels[bbox.zoom];
		if ((s.usedMask & (1u << bbox.zoom)) && !(level.index == bbox.index)) {
			// Can't happen: local() just dropped every level >= bbox.zoom unless
			// bbox is the current tile. Kept so a level only ever holds one tile.
			s.total -= level.m.size();
			level.m.clear();
		}
		s.usedMask |= (1u << bbox.zoom);
		level.index = bbox.index;
		auto ins = level.m.emplace(Key{(const void*)this, objectID}, Entry{copy, valid});
		if (ins.second)
			s.total++;
		else
			ins.first->second = Entry{copy, valid};
	}

private:
	static const uint MAX_LEVELS = 32;

	// Entries of one ClipCache instance (`owner`) for one object.
	struct Key {
		const void* owner;
		NodeID id;
		bool operator==(const Key& o) const { return owner == o.owner && id == o.id; }
	};
	struct KeyHash {
		size_t operator()(const Key& k) const {
			uint64_t h = (uint64_t)k.id * 0x9E3779B97F4A7C15ull ^ (uint64_t)(uintptr_t)k.owner;
			h ^= h >> 29;
			h *= 0xBF58476D1CE4E5B9ull;
			h ^= h >> 32;
			return (size_t)h;
		}
	};
	// All entries stored at one zoom. Only one tile per zoom can be live at a
	// time (see below), so each level holds a single tile index.
	struct Level {
		TileCoordinates index = TileCoordinates(0, 0);
		std::unordered_map<Key, Entry, KeyHash> m;
	};

	// Per-thread store, shared by all ClipCache instances on the thread (hence
	// keyed on `this`), cleared when the thread starts a new batch.
	// Tiles within a batch come in depth-first order, so when a new tile at
	// zoom z starts, no entry at zoom >= z can be an ancestor of it or of any
	// later tile: those are dropped, leaving only the current tile's ancestors.
	struct LocalStore {
		uint64_t gen = ~0ull;
		uint zoom = 0;
		TileCoordinates index = TileCoordinates(0, 0);
		uint32_t usedMask = 0;
		size_t total = 0;
		Level levels[MAX_LEVELS];
#ifdef TMOPT_VERIFY
		std::map<std::tuple<uint16_t, const void*, TileCoordinates, NodeID>, Entry> shadow;
#endif

		// Drop every level >= zoom.
		void clearFrom(uint zoom) {
			for (uint z = zoom; z < MAX_LEVELS; z++) {
				if (usedMask & (1u << z)) {
					total -= levels[z].m.size();
					levels[z].m.clear();
				}
			}
			usedMask &= zoom >= MAX_LEVELS ? ~0u : ((1u << zoom) - 1);
		}
	};
	static LocalStore& local(uint zoom, TileCoordinates index) {
		static thread_local LocalStore s;
		if (s.gen != clipCacheBatchId) {
			s.clearFrom(0);
#ifdef TMOPT_VERIFY
			s.shadow.clear();
#endif
			s.gen = clipCacheBatchId;
		} else if (s.zoom == zoom && s.index == index) {
			return s;
		}
		s.zoom = zoom;
		s.index = index;
		s.clearFrom(zoom);
#ifdef TMOPT_VERIFY
		s.shadow.erase(s.shadow.lower_bound(std::make_tuple((uint16_t)zoom, (const void*)nullptr, TileCoordinates(0, 0), (NodeID)0)), s.shadow.end());
#endif
		return s;
	}

	unsigned int baseZoom;
};

#endif
