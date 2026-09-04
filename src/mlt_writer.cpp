#include "mlt_writer.h"

#include <mlt/encoder.hpp>
#include <mlt/metadata/tileset.hpp>

#include <cstdint>
#include <utility>
#include <vector>

#ifndef MLT_VERSION
#define MLT_VERSION (unknown)
#endif
#define STR1(x)  #x
#define STR(x)  STR1(x)

namespace MltWriter {

bool isAvailable() {
	return true;
}

std::string version() {
	return STR(MLT_VERSION);
}

std::string encodeSampleTile() {
	mlt::Encoder::Feature feature;
	feature.id = 1;
	feature.geometry.type = mlt::metadata::tileset::GeometryType::POINT;
	feature.geometry.coordinates.push_back({0, 0});
	feature.properties.emplace("name", std::string("tilemaker"));

	mlt::Encoder::Layer layer;
	layer.name = "test";
	layer.extent = 4096;
	layer.features.push_back(std::move(feature));

	const mlt::Encoder encoder;
	const std::vector<std::uint8_t> tile = encoder.encode({layer});
	return std::string(tile.begin(), tile.end());
}

}
