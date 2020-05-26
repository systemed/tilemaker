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

inline std::vector<std::string> split_string(std::string &inputStr, char sep) {
	std::stringstream ss(inputStr);
	std::string item;
	std::vector<std::string> res;
	while (std::getline(ss, item, sep)) { res.push_back(item); }
	return res;
}

std::string decompress_string(const std::string& str);

std::string compress_string(const std::string& str,
                            int compressionlevel = Z_DEFAULT_COMPRESSION,
                            bool asGzip = false);

std::string boost_validity_error(unsigned failure);

extern const double CLIPPER_SCALE;

void ConvertToClipper(const Polygon &p, ClipperLib::Path &outer, ClipperLib::Paths &inners);
void ConvertToClipper(const MultiPolygon &mp, ClipperLib::Paths &out);
void ConvertToClipper(const MultiPolygon &mp, ClipperLib::PolyTree &out);
void ConvertFromClipper(const ClipperLib::Path &outer, const ClipperLib::Paths &inners, Polygon &p);
void ConvertFromClipper(const ClipperLib::PolyTree &pt, MultiPolygon &mp);

#endif //_HELPERS_H
