/*! \file */
#ifndef _LUA_POOL_H
#define _LUA_POOL_H

// Size-class pool allocator for a lua_State (lua_Alloc).
//
// Lua makes a very large number of small, short-lived allocations (strings,
// tables, closures) while running way_function etc. Each lua_State belongs to
// one OsmLuaProcessing object; it is never used by two threads at once
// (per-thread states are thread_local; the main-thread state is only used
// from the main thread or under the shapefile/GeoJSON attributeMutex), so the
// pool needs no locks.
//
// Blocks of up to MaxSmall bytes are carved from 64 KB slabs in 16-byte size
// classes and recycled through per-class free lists. Larger blocks go to
// malloc/realloc/free. Slabs are only released when the pool is destroyed,
// which must happen after lua_close().

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <vector>
#ifdef TMOPT_VERIFY
#include <unordered_map>
#include <iostream>
#endif

class LuaPool {
public:
	static const size_t Granule = 16;
	static const size_t MaxSmall = 512;
	static const size_t NumClasses = MaxSmall / Granule;
	static const size_t SlabSize = 64 * 1024;

	LuaPool() : bump(nullptr), bumpEnd(nullptr) {
		for (size_t i = 0; i < NumClasses; i++) freeLists[i] = nullptr;
	}
	~LuaPool() {
		for (void *slab : slabs) std::free(slab);
	}

	// lua_Alloc. When ptr==NULL, osize encodes the object type, not a size.
	static void *alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
		LuaPool &pool = *static_cast<LuaPool*>(ud);
		if (ptr && osize == 0) {
			// Lua always passes the block size; only a misbehaving C module (via lua_getallocf) gets here
			std::fputs("LuaPool: allocator called with osize 0 (C module using lua_getallocf incorrectly)\n", stderr);
			std::abort();
		}
		if (nsize == 0) {
			if (ptr) pool.release(ptr, osize);
			return nullptr;
		}
		if (!ptr) return pool.allocate(nsize);

		bool oSmall = osize <= MaxSmall, nSmall = nsize <= MaxSmall;
		if (oSmall && nSmall && classOf(osize) == classOf(nsize)) {
#ifdef TMOPT_VERIFY
			pool.verifyFree(ptr, osize);
			pool.live[ptr] = nsize;
#endif
			return ptr;
		}
		if (!oSmall && !nSmall) return std::realloc(ptr, nsize);

		void *n = pool.allocate(nsize);
		if (!n) return nsize <= osize ? ptr : nullptr; // shrinking must not fail (Lua 5.1/5.2 assume it); else Lua keeps the old block
		std::memcpy(n, ptr, osize < nsize ? osize : nsize);
		pool.release(ptr, osize);
		return n;
	}

private:
	struct FreeBlock { FreeBlock *next; };

	LuaPool(const LuaPool&);
	LuaPool& operator=(const LuaPool&);

	static size_t classOf(size_t n) { return (n - 1) / Granule; } // n in [1, MaxSmall]

	void *allocate(size_t n) {
		if (n > MaxSmall) return std::malloc(n);
		size_t c = classOf(n);
		void *p;
		if (freeLists[c]) {
			FreeBlock *b = freeLists[c];
			freeLists[c] = b->next;
			p = b;
		} else {
			size_t bytes = (c + 1) * Granule;
			if (bump == nullptr || (size_t)(bumpEnd - bump) < bytes) {
				char *slab = static_cast<char*>(std::malloc(SlabSize));
				if (!slab) return nullptr;
				try { slabs.push_back(slab); }
				catch (...) { std::free(slab); return nullptr; }
				bump = slab;
				bumpEnd = slab + SlabSize;
			}
			p = bump;
			bump += bytes;
		}
#ifdef TMOPT_VERIFY
		live[p] = n;
#endif
		return p;
	}

	void release(void *p, size_t osize) {
		if (osize > MaxSmall) { std::free(p); return; }
#ifdef TMOPT_VERIFY
		verifyFree(p, osize);
		live.erase(p);
#endif
		size_t c = classOf(osize);
		FreeBlock *b = static_cast<FreeBlock*>(p);
		b->next = freeLists[c];
		freeLists[c] = b;
	}

#ifdef TMOPT_VERIFY
	// Lua must hand back every small block with the size it was last given.
	void verifyFree(void *p, size_t osize) {
		auto it = live.find(p);
		if (it == live.end() || it->second != osize) {
			std::cerr << "TMOPT_VERIFY LuaPool: bad free " << p << " osize " << osize
			          << " recorded " << (it == live.end() ? 0 : it->second) << std::endl;
			std::abort();
		}
	}
	std::unordered_map<void*, size_t> live;
#endif

	FreeBlock *freeLists[NumClasses];
	char *bump, *bumpEnd;
	std::vector<void*> slabs;
};

#endif //_LUA_POOL_H
