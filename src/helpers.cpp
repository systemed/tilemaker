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
#include "external/libdeflate/libdeflate.h"
#include "helpers.h"

#ifdef _MSC_VER
#define stat64 __stat64
#endif

#if defined(__APPLE__)
#define stat64 stat
#endif

#define MOD_GZIP_ZLIB_WINDOWSIZE 15
#define MOD_GZIP_ZLIB_CFACTOR 9
#define MOD_GZIP_ZLIB_BSIZE 8096

using namespace std;

class Compressor {
public:
	int level;
	libdeflate_compressor* compressor;

	Compressor(int level): level(level), compressor(NULL) {
		compressor = libdeflate_alloc_compressor(level);

		if (!compressor)
			throw std::runtime_error("libdeflate_alloc_compressor failed (level=" + std::to_string(level) + ")");
	}

	~Compressor() {
		libdeflate_free_compressor(compressor);
	}
};

class Decompressor {
public:
	libdeflate_decompressor* decompressor;

	Decompressor(): decompressor(NULL) {
		decompressor = libdeflate_alloc_decompressor();

		if (!decompressor)
			throw std::runtime_error("libdeflate_alloc_decompressor failed");
	}

	~Decompressor() {
		libdeflate_free_decompressor(decompressor);
	}
};


thread_local Compressor compressor(6);
thread_local Decompressor decompressor;

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

// Compress a STL string using zlib with given compression level, and return the binary data
// TODO: consider returning a std::vector<char> ?
std::string compress_string(const std::string& str,
                            int compressionlevel,
                            bool asGzip) {
	if (compressionlevel == Z_DEFAULT_COMPRESSION)
		compressionlevel = 6;

	if (compressionlevel != compressor.level)
		compressor = Compressor(compressionlevel);

	std::string rv;
	if (asGzip) {
		size_t maxSize = libdeflate_gzip_compress_bound(compressor.compressor, str.size());
		rv.resize(maxSize);

		size_t compressedSize = libdeflate_gzip_compress(compressor.compressor, str.data(), str.size(), &rv[0], maxSize);
		if (compressedSize == 0)
			throw std::runtime_error("libdeflate_gzip_compress failed");
		rv.resize(compressedSize);
	} else {
		size_t maxSize = libdeflate_zlib_compress_bound(compressor.compressor, str.size());
		rv.resize(maxSize);

		size_t compressedSize = libdeflate_zlib_compress(compressor.compressor, str.data(), str.size(), &rv[0], maxSize);
		if (compressedSize == 0)
			throw std::runtime_error("libdeflate_zlib_compress failed");
		rv.resize(compressedSize);
	}

	return rv;
}

// Decompress an STL string using zlib and return the original data.
// The output buffer is passed in; callers are meant to re-use the buffer such
// that eventually no allocations are needed when decompressing.
void decompress_string(std::string& output, const char* input, uint32_t inputSize, bool asGzip) {
	size_t uncompressedSize;

	if (output.size() < inputSize)
		output.resize(inputSize);

	while (true) {
		libdeflate_result rv = LIBDEFLATE_BAD_DATA;

		if (asGzip) {
			rv = libdeflate_gzip_decompress(
				decompressor.decompressor,
				input,
				inputSize,
				&output[0],
				output.size(),
				&uncompressedSize
			);
		} else {
			rv = libdeflate_zlib_decompress(
				decompressor.decompressor,
				input,
				inputSize,
				&output[0],
				output.size(),
				&uncompressedSize
			);
		}

		if (rv == LIBDEFLATE_SUCCESS) {
			output.resize(uncompressedSize);
			return;
		}

		if (rv == LIBDEFLATE_INSUFFICIENT_SPACE) {
			output.resize((output.size() + 128) * 2);
		} else
			throw std::runtime_error("libdeflate_gzip_decompress failed");
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
