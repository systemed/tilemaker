/*! \file */
#ifndef _MLT_WRITER_H
#define _MLT_WRITER_H

#include <mlt/encoder.hpp>
#include <optional>
#include <string>
#include <vector>

// Builders that collect the features of one tile for the MapLibre Tile (MLT) encoder.
// They mirror the subset of the vtzero builder API that tile_worker.cpp uses, so the
// same templated code writes either MVT or MLT.

class MltLayerBuilder {
public:
	MltLayerBuilder(const std::string& name, uint32_t extent) {
		layer.name = name;
		layer.extent = extent;
	}

	mlt::Encoder::Layer layer;
};

class MltFeatureBuilder {
public:
	explicit MltFeatureBuilder(MltLayerBuilder& layerBuilder) : layerBuilder(layerBuilder) {
		feature.id = std::nullopt;
	}

	void set_id(uint64_t id) { feature.id = id; }

	void add_property(const std::string& key, const std::string& value) { feature.properties.emplace(key, value); }
	void add_property(const std::string& key, bool value) { feature.properties.emplace(key, value); }
	void add_property(const std::string& key, int value) { feature.properties.emplace(key, static_cast<int32_t>(value)); }
	void add_property(const std::string& key, float value) { feature.properties.emplace(key, value); }

protected:
	void commitFeature() { layerBuilder.layer.features.push_back(std::move(feature)); }

	MltLayerBuilder& layerBuilder;
	mlt::Encoder::Feature feature;
};

class MltPointFeatureBuilder : public MltFeatureBuilder {
public:
	using MltFeatureBuilder::MltFeatureBuilder;

	void add_points(uint32_t count) { feature.geometry.coordinates.reserve(count); }
	void set_point(int x, int y) { feature.geometry.coordinates.push_back({x, y}); }

	void commit() {
		auto& g = feature.geometry;
		g.type = g.coordinates.size() == 1 ? mlt::Encoder::GeometryType::POINT : mlt::Encoder::GeometryType::MULTIPOINT;
		commitFeature();
	}
};

class MltLinestringFeatureBuilder : public MltFeatureBuilder {
public:
	using MltFeatureBuilder::MltFeatureBuilder;

	void add_linestring(uint32_t count) {
		feature.geometry.parts.emplace_back();
		feature.geometry.parts.back().reserve(count);
	}
	void set_point(int x, int y) { feature.geometry.parts.back().push_back({x, y}); }

	void commit() {
		auto& g = feature.geometry;
		if (g.parts.size() == 1) {
			g.type = mlt::Encoder::GeometryType::LINESTRING;
			g.coordinates = std::move(g.parts.front());
			g.parts.clear();
		} else {
			g.type = mlt::Encoder::GeometryType::MULTILINESTRING;
		}
		commitFeature();
	}
};

class MltPolygonFeatureBuilder : public MltFeatureBuilder {
public:
	using MltFeatureBuilder::MltFeatureBuilder;

	// count includes the closing point, as with vtzero; MLT rings are stored without it
	void add_ring(uint32_t count, bool outer) {
		auto& g = feature.geometry;
		if (outer) {
			g.parts.emplace_back();
			g.partRingSizes.emplace_back();
		}
		g.partRingSizes.back().push_back(count - 1);
		ringPointsLeft = count;
	}
	void set_point(int x, int y) {
		if (--ringPointsLeft > 0) feature.geometry.parts.back().push_back({x, y});
	}

	void commit() {
		auto& g = feature.geometry;
		if (g.parts.size() == 1) {
			g.type = mlt::Encoder::GeometryType::POLYGON;
			g.coordinates = std::move(g.parts.front());
			g.ringSizes = std::move(g.partRingSizes.front());
			g.parts.clear();
			g.partRingSizes.clear();
		} else {
			g.type = mlt::Encoder::GeometryType::MULTIPOLYGON;
		}
		commitFeature();
	}

private:
	uint32_t ringPointsLeft = 0;
};

/// Encode the collected layers as an MLT tile. Empty layers are skipped.
/// Set compressed when the tile will be gzip/deflate compressed afterwards.
void encodeMltTile(std::vector<mlt::Encoder::Layer>& layers, bool includeIds, bool compressed, std::string& output);

#endif //_MLT_WRITER_H
