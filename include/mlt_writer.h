/*! \file */
#ifndef _MLT_WRITER_H
#define _MLT_WRITER_H

#include <string>

/*
	Bridge to the MapLibre Tiles (MLT) encoder from
	https://github.com/maplibre/maplibre-tile-spec

	The MLT encoder requires C++20, while the rest of tilemaker is built as
	C++14. src/mlt_writer.cpp is the only translation unit compiled against
	MLT's headers, so nothing declared here may expose an MLT type, and all
	MLT code belongs behind this interface.

	Builds without MLT support link src/mlt_writer_stub.cpp instead, so these
	functions may be called unconditionally: ask isAvailable() at runtime
	rather than testing a macro.
*/
namespace MltWriter {

	/// Whether this build can encode MLT tiles.
	bool isAvailable();

	/// Revision of the bundled MLT encoder, or "" if unavailable.
	std::string version();

	/// Encode a one-feature tile and return it as raw MLT bytes, or "" if
	/// unavailable. Exercises the encoder end-to-end so that a build can be
	/// verified before the real geometry conversion exists.
	std::string encodeSampleTile();
}

#endif //_MLT_WRITER_H
