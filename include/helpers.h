/*! \file */ 
#ifndef _HELPERS_H
#define _HELPERS_H

#include <zlib.h>
#include "clipper.hpp"
#include "geomtypes.h"

// General helper routines

inline void endian_swap(unsigned int& x) {
	x = (x>>24) | 
        ((x<<8) & 0x00FF0000) |
        ((x>>8) & 0x0000FF00) |
        (x<<24);
}

inline bool ends_with(std::string const & value, std::string const & ending) {
	if (ending.size() > value.size()) return false;
	return std::equal(ending.rbegin(), ending.rend(), value.rbegin());
}

std::string decompress_string(const std::string& str);

std::string compress_string(const std::string& str,
                            int compressionlevel = Z_DEFAULT_COMPRESSION,
                            bool asGzip = false);

extern const double CLIPPER_SCALE;

void ConvertToClipper(const Polygon &p, ClipperLib::Path &outer, ClipperLib::Paths &inners);
void ConvertToClipper(const MultiPolygon &mp, ClipperLib::Paths &out);
void ConvertFromClipper(const ClipperLib::Path &outer, const ClipperLib::Paths &inners, Polygon &p);
void ConvertFromClipper(const ClipperLib::Paths &polys, MultiPolygon &mp);


#endif //_HELPERS_H
