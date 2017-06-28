#include <string>
#include <stdexcept>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cstring>
#include <zlib.h>

#define MOD_GZIP_ZLIB_WINDOWSIZE 15
#define MOD_GZIP_ZLIB_CFACTOR 9
#define MOD_GZIP_ZLIB_BSIZE 8096

#include "clipper.hpp"
using namespace ClipperLib;

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

// zlib routines from http://panthema.net/2007/0328-ZLibString.html

// Compress a STL string using zlib with given compression level, and return the binary data
std::string compress_string(const std::string& str,
                            int compressionlevel = Z_DEFAULT_COMPRESSION,
                            bool asGzip = false) {
    z_stream zs;                        // z_stream is zlib's control structure
    memset(&zs, 0, sizeof(zs));

	if (asGzip) {
		if (deflateInit2(&zs, compressionlevel, Z_DEFLATED,
		                MOD_GZIP_ZLIB_WINDOWSIZE + 16, MOD_GZIP_ZLIB_CFACTOR, Z_DEFAULT_STRATEGY) != Z_OK)
	        throw(std::runtime_error("deflateInit2 failed while compressing."));
	} else {
	    if (deflateInit(&zs, compressionlevel) != Z_OK)
	        throw(std::runtime_error("deflateInit failed while compressing."));
	}

    zs.next_in = (Bytef*)str.data();
    zs.avail_in = str.size();           // set the z_stream's input

    int ret;
    char outbuffer[32768];
    std::string outstring;

    // retrieve the compressed bytes blockwise
    do {
        zs.next_out = reinterpret_cast<Bytef*>(outbuffer);
        zs.avail_out = sizeof(outbuffer);

        ret = deflate(&zs, Z_FINISH);

        if (outstring.size() < zs.total_out) {
            // append the block to the output string
            outstring.append(outbuffer,
                             zs.total_out - outstring.size());
        }
    } while (ret == Z_OK);

    deflateEnd(&zs);

    if (ret != Z_STREAM_END) {          // an error occurred that was not EOF
        std::ostringstream oss;
        oss << "Exception during zlib compression: (" << ret << ") " << zs.msg;
        throw(std::runtime_error(oss.str()));
    }

    return outstring;
}

// Decompress an STL string using zlib and return the original data.
std::string decompress_string(const std::string& str) {
    z_stream zs;                        // z_stream is zlib's control structure
    memset(&zs, 0, sizeof(zs));

    if (inflateInit(&zs) != Z_OK)
        throw(std::runtime_error("inflateInit failed while decompressing."));

    zs.next_in = (Bytef*)str.data();
    zs.avail_in = str.size();

    int ret;
    char outbuffer[32768];
    std::string outstring;

    // get the decompressed bytes blockwise using repeated calls to inflate
    do {
        zs.next_out = reinterpret_cast<Bytef*>(outbuffer);
        zs.avail_out = sizeof(outbuffer);

        ret = inflate(&zs, 0);

        if (outstring.size() < zs.total_out) {
            outstring.append(outbuffer,
                             zs.total_out - outstring.size());
        }

    } while (ret == Z_OK);

    inflateEnd(&zs);

    if (ret != Z_STREAM_END) {          // an error occurred that was not EOF
        std::ostringstream oss;
        oss << "Exception during zlib decompression: (" << ret << ") "
            << zs.msg;
        throw(std::runtime_error(oss.str()));
    }

    return outstring;
}

const double clipperScale = 1e6;

void ConvertToClipper(const Polygon &p, Path &outer, Paths &inners)
{
	outer.clear();
	inners.clear();
	const Polygon::ring_type &out = p.outer();
	const Polygon::inner_container_type &inns = p.inners();

	for(size_t i=0; i<out.size(); i++)
	{
		const Point &pt = out[i];
		outer.push_back(IntPoint(std::round(pt.x() * clipperScale), std::round(pt.y() * clipperScale)));
	}

	for(size_t i=0; i<inns.size(); i++)
	{
		Path in;
		const Polygon::ring_type &inner = inns[i];
		for(size_t j=0; j<inner.size(); j++)
		{
			const Point &pt = inner[j];
			in.push_back(IntPoint(std::round(pt.x() * clipperScale), std::round(pt.y() * clipperScale)));
		}
		inners.push_back(in);
	}
}

void ConvertFromClipper(const Path &outer, const Paths &inners, Polygon &p)
{
	p.clear();
	Polygon::ring_type &out = p.outer();
	Polygon::inner_container_type &inns = p.inners();
	
	for(size_t i=0; i<outer.size(); i++)
	{
		const IntPoint &pt = outer[i];
		out.push_back(Point(pt.X, pt.Y));
	}

	for(size_t i=0; i<inners.size(); i++)
	{
		const Path &inn = inners[i];
		Polygon::ring_type inn2;
		for(size_t j=0; j<inn.size(); j++)
		{
			const IntPoint &pt = inn[j];
			inn2.push_back(Point(pt.X / clipperScale, pt.Y / clipperScale));
		}
		inns.push_back(inn2);
	}
}

