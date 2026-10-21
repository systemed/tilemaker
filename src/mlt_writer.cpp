/*! \file */
#include "mlt_writer.h"
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace {

// MLT stores each property as a typed column per layer, and the encoder falls back to a
// string column when a key holds both integers and floats. In MVT every value carries its
// own type, so profiles legitimately mix them (e.g. AttributeInteger and AttributeNumeric
// for the same key). Promote the integers to double so such columns stay numeric.
void widenMixedNumericColumns(mlt::Encoder::Layer& layer) {
	constexpr uint8_t SEEN_INT = 1, SEEN_FLOAT = 2;
	std::unordered_map<std::string_view, uint8_t> seen;
	for (const auto& feature : layer.features) {
		for (const auto& [key, value] : feature.properties) {
			if (std::holds_alternative<int32_t>(value)) seen[key] |= SEEN_INT;
			else if (std::holds_alternative<float>(value)) seen[key] |= SEEN_FLOAT;
		}
	}

	std::unordered_set<std::string> mixed;
	for (const auto& [key, kinds] : seen) {
		if (kinds == (SEEN_INT | SEEN_FLOAT)) mixed.emplace(key);
	}
	if (mixed.empty()) return;

	for (auto& feature : layer.features) {
		for (const auto& key : mixed) {
			auto it = feature.properties.find(key);
			if (it != feature.properties.end() && std::holds_alternative<int32_t>(it->second)) {
				it->second = static_cast<double>(std::get<int32_t>(it->second));
			}
		}
	}
}

} // namespace

void encodeMltTile(std::vector<mlt::Encoder::Layer>& layers, bool includeIds, bool compressed, std::string& output) {
	thread_local const mlt::Encoder encoder;

	const auto config = mlt::EncoderConfig::with([&](mlt::EncoderConfig& c) {
		c.includeIds = includeIds;
		// Feature order within a layer carries the profile's ZOrder, so it must be preserved
		c.sortFeatures = false;
		if (compressed) {
			// The encoder's size-driven choices (FSST strings, Morton/Hilbert vertex dictionaries,
			// per-stream integer encodings for geometry) make up most of its time, but once the tile
			// is gzip/deflate compressed they save little or nothing. Encoding geometry with plain
			// varints skips the dictionary candidates entirely.
			c.useFsst = false;
			c.enableMortonEncoding = false;
			c.geometryEncodingOption = mlt::IntegerEncodingOption::PLAIN;
		}
	});

	for (auto& layer : layers) widenMixedNumericColumns(layer);

	std::vector<uint8_t> encoded = encoder.encode(layers, config);
	output.assign(reinterpret_cast<const char*>(encoded.data()), encoded.size());
}
