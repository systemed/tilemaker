/*! \file */
#ifndef _STABLE_HASH_H
#define _STABLE_HASH_H

#include <cstddef>
#include <cstdint>
#include <cstring>

// Hash functions that give the same result on every platform, compiler and
// library version (unlike std::hash or boost::hash), for orderings that the
// output depends on.

inline uint64_t stableMix(uint64_t x) {
	// splitmix64 finaliser
	x += 0x9e3779b97f4a7c15ull;
	x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
	x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
	return x ^ (x >> 31);
}

inline uint64_t stableCombine(uint64_t seed, uint64_t value) {
	return stableMix(seed ^ stableMix(value));
}

inline uint64_t stableCombine(uint64_t seed, double value) {
	if (value == 0) value = 0; // -0.0 and 0.0 compare equal, so hash them equally
	uint64_t bits;
	std::memcpy(&bits, &value, sizeof(bits));
	return stableCombine(seed, bits);
}

inline uint64_t stableCombine(uint64_t seed, const char* data, std::size_t length) {
	// FNV-1a over the bytes
	uint64_t h = 0xcbf29ce484222325ull;
	for (std::size_t i = 0; i < length; i++) {
		h ^= static_cast<unsigned char>(data[i]);
		h *= 0x100000001b3ull;
	}
	return stableCombine(stableCombine(seed, static_cast<uint64_t>(length)), h);
}

#endif //_STABLE_HASH_H
