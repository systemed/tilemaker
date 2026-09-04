#include "mlt_writer.h"

// Linked in place of mlt_writer.cpp when tilemaker is built without MLT
// support; see include/mlt_writer.h.

namespace MltWriter {

bool isAvailable() {
	return false;
}

std::string version() {
	return "";
}

std::string encodeSampleTile() {
	return "";
}

}
