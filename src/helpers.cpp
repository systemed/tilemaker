#include <string>
#include <stdexcept>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cstring>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>

#include <sys/stat.h>
#include "helpers.h"

#ifdef _MSC_VER
#define stat64 __stat64
#endif

#define MOD_GZIP_ZLIB_WINDOWSIZE 15
#define MOD_GZIP_ZLIB_CFACTOR 9
#define MOD_GZIP_ZLIB_BSIZE 8096

using namespace std;

// Bounding box string parsing

double bboxElementFromStr(const std::string& number) {
	try {
		return boost::lexical_cast<double>(number);
	} catch (boost::bad_lexical_cast&) {
		std::cerr << "Failed to parse coordinate " << number << std::endl;
		exit(1);
	}
}

// Split bounding box provided as a comma-separated list of coordinates.
std::vector<std::string> parseBox(const std::string& bbox) {
	std::vector<std::string> bboxParts;
	if (!bbox.empty()) {
		boost::split(bboxParts, bbox, boost::is_any_of(","));
		if (bboxParts.size() != 4) {
			std::cerr << "Bounding box must contain 4 elements: minlon,minlat,maxlon,maxlat" << std::endl;
			exit(1);
		}
	}
	return bboxParts;
}

// zlib routines from http://panthema.net/2007/0328-ZLibString.html

// Compress a STL string using zlib with given compression level, and return the binary data
std::string compress_string(const std::string& str,
                            int compressionlevel,
                            bool asGzip) {
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
// The output buffer is passed in; callers are meant to re-use the buffer such
// that eventually no allocations are needed when decompressing.
void decompress_string(std::string& output, const char* input, uint32_t inputSize, bool asGzip) {
    z_stream zs;                        // z_stream is zlib's control structure
    memset(&zs, 0, sizeof(zs));

	if (asGzip) {
		if (inflateInit2(&zs, 16+MAX_WBITS) != Z_OK)
			throw(std::runtime_error("inflateInit2 failed while decompressing."));
	} else {
		if (inflateInit(&zs) != Z_OK)
			throw(std::runtime_error("inflateInit failed while decompressing."));
	}

    zs.next_in = (Bytef*)input;
    zs.avail_in = inputSize;

    int ret;

    int actualOutputSize = 0;

    // get the decompressed bytes blockwise using repeated calls to inflate
    do {
        if (output.size() < actualOutputSize + 32768)
            output.resize(actualOutputSize + 32768);

        zs.next_out = reinterpret_cast<Bytef*>(&output[actualOutputSize]);
        zs.avail_out = output.size() - actualOutputSize;

        ret = inflate(&zs, 0);

        actualOutputSize = zs.total_out;
    } while (ret == Z_OK);

    output.resize(actualOutputSize);
    inflateEnd(&zs);

    if (ret != Z_STREAM_END) {          // an error occurred that was not EOF
        std::ostringstream oss;
        oss << "Exception during zlib decompression: (" << ret << ") "
            << zs.msg;
        throw(std::runtime_error(oss.str()));
    }
}

// Parse a Boost error
std::string boost_validity_error(unsigned failure) {
	switch (failure) {
		case 10: return "too few points";
		case 11: return "wrong topological dimension";
		case 12: return "spikes (nodes go back on themselves)";
		case 13: return "consecutive duplicate points";
		case 20: return "not been closed";
		case 21: return "self-intersections";
		case 22: return "the wrong orientation";
		case 30: return "interior rings outside";
		case 31: return "nested interior rings";
		case 32: return "disconnected interior (contains polygons whose interiors are not disjoint)";
		case 40: return "intersecting interiors";
		default: return "something mysterious wrong with it, Boost validity_failure_type " + to_string(failure);
	}
}

uint64_t getFileSize(std::string filename) {
	struct stat64 statBuf;
	int rc = stat64(filename.c_str(), &statBuf);

	if (rc == 0) return statBuf.st_size;

	throw std::runtime_error("unable to stat " + filename);
}

// Given a file, attempt to divide it into N chunks, with each chunk separated
// by a newline.
//
// Useful for dividing a JSON lines file into blocks suitable for parallel processing.
std::vector<OffsetAndLength> getNewlineChunks(const std::string &filename, uint64_t chunks) {
	std::vector<OffsetAndLength> rv;

	const uint64_t size = getFileSize(filename);
	const uint64_t chunkSize = std::max<uint64_t>(size / chunks, 1ul);
	FILE* fp = fopen(filename.c_str(), "r");

	// Our approach is naive: skip chunkSize bytes, scan for a newline, repeat.
	//
	// Per UTF-8's ascii transparency property, a newline is guaranteed not to form
	// part of any multi-byte character, so the byte '\n' reliably indicates a safe
	// place to start a new chunk.
	uint64_t offset = 0;
	uint64_t length = 0;
	char buffer[8192];
	while (offset < size) {
		// The last chunk will not be a full `chunkSize`.
		length = std::min(chunkSize, size - offset);

		if (fseek(fp, offset + length, SEEK_SET) != 0) throw std::runtime_error("unable to seek to " + std::to_string(offset) + " in " + filename);

		bool foundNewline = false;

		while(!foundNewline) {
			size_t read = fread(buffer, 1, sizeof(buffer), fp);
			if (read == 0) break;
			for (int i = 0; i < read; i++) {
				if (buffer[i] == '\n') {
					length += i;
					foundNewline = true;
					break;
				}
			}

			if (!foundNewline) length += read;
    }

		rv.push_back({offset, length});
		offset += length;
	}

	fclose(fp);
	return rv;
}
