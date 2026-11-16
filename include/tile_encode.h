/*! \file */
#ifndef _TILE_ENCODE_H
#define _TILE_ENCODE_H

// Encoding of one feature (geometry in tile coordinates and attributes) into a layer of the
// tile. Shared by tile_worker.cpp and the offline replay of recorded tiles (mvt_record.h).

#include <vector>
#include <utility>
#include <memory>
#include <cstring>
#include <type_traits>
#include <algorithm>
#include <array>
#include <vtzero/builder.hpp>
#include "coordinates_geom.h"
#include "output_object.h"

// The tile is written through one of these backends. Both expose the same builder API,
// so the geometry and attribute code below is shared between the output formats.
struct MvtBackend {
	using LayerBuilder = vtzero::layer_builder;
	using PointFeature = vtzero::point_feature_builder;
	using LinestringFeature = vtzero::linestring_feature_builder;
	using PolygonFeature = vtzero::polygon_feature_builder;
	static void addRing(PolygonFeature& fbuilder, uint32_t points, bool /*outer*/) { fbuilder.add_ring(points); }
};

// Direct encoding of MVT features: the Feature message is written into a per-thread buffer and
// added to the layer in one go, rather than through the vtzero feature builders (which write each
// varint byte by byte and reserve/erase the length of three nested messages per feature). The
// bytes are exactly those vtzero writes: fields in the order type, id, geometry, tags; the
// geometry commands, zigzag deltas and minimal length varints as vtzero/protozero encode them.
namespace mvt_direct {

struct Buffer {
	std::unique_ptr<char[]> data;
	std::size_t size = 0, capacity = 0;
	// Room for at least n more bytes; returns the write position
	char* reserve(std::size_t n) {
		if (size + n > capacity) {
			std::size_t c = capacity ? capacity * 2 : 4096;
			while (c < size + n) c *= 2;
			std::unique_ptr<char[]> d(new char[c]);
			if (size) std::memcpy(d.get(), data.get(), size);
			data = std::move(d);
			capacity = c;
		}
		return data.get() + size;
	}
};

inline char* putVarint(char* p, uint64_t v) {
	while (v >= 0x80U) { *p++ = char((v & 0x7fU) | 0x80U); v >>= 7U; }
	*p++ = char(v);
	return p;
}
inline uint32_t zigzag(int32_t v) { return protozero::encode_zigzag32(v); }

// Geometry of the feature being encoded: command integers and zigzag deltas, as varints
struct Geometry {
	Buffer buf;
	vtzero::point cursor{0, 0};
	void clear() { buf.size = 0; cursor = vtzero::point{0, 0}; }
	// write position with room for maxVarints varints
	char* begin(std::size_t maxVarints) { return buf.reserve(maxVarints * 5); }
	void end(char* p) { buf.size = p - buf.data.get(); }
	char* delta(char* p, int32_t x, int32_t y) {
		p = putVarint(p, zigzag(x - cursor.x));
		p = putVarint(p, zigzag(y - cursor.y));
		cursor = vtzero::point{x, y};
		return p;
	}
};

struct Scratch {
	Geometry geometry;
	Buffer feature;
	std::vector<std::pair<int, int>> points;
};
inline Scratch& scratch() { thread_local Scratch s; return s; }

// Writes the feature (type, id, the geometry in s.geometry, the tags kv) into the layer
inline void commit(vtzero::layer_builder& vtLayer, vtzero::GeomType type, bool withId, uint64_t id,
                   const uint32_t* kv, uint32_t kvCount, Scratch& s) {
	const std::size_t geomSize = s.geometry.buf.size;
	std::size_t tagsSize = 0;
	for (uint32_t i = 0; i < 2 * kvCount; i++) {
		uint32_t v = kv[i];
		tagsSize++;
		while (v >= 0x80U) { tagsSize++; v >>= 7U; }
	}
	Buffer& f = s.feature;
	f.size = 0;
	char* const start = f.reserve(2 + 11 + 6 + geomSize + 6 + tagsSize);
	char* p = start;
	*p++ = char((uint32_t(vtzero::detail::pbf_feature::type) << 3U) | uint32_t(protozero::pbf_wire_type::varint));
	p = putVarint(p, uint64_t(int64_t(static_cast<int32_t>(type))));
	if (withId) {
		*p++ = char((uint32_t(vtzero::detail::pbf_feature::id) << 3U) | uint32_t(protozero::pbf_wire_type::varint));
		p = putVarint(p, id);
	}
	*p++ = char((uint32_t(vtzero::detail::pbf_feature::geometry) << 3U) | uint32_t(protozero::pbf_wire_type::length_delimited));
	p = putVarint(p, geomSize);
	std::memcpy(p, s.geometry.buf.data.get(), geomSize);
	p += geomSize;
	if (kvCount) {
		*p++ = char((uint32_t(vtzero::detail::pbf_feature::tags) << 3U) | uint32_t(protozero::pbf_wire_type::length_delimited));
		p = putVarint(p, tagsSize);
		for (uint32_t i = 0; i < 2 * kvCount; i++) p = putVarint(p, kv[i]);
	}
	vtLayer.add_encoded_feature(start, p - start);
}

} // namespace mvt_direct

// Points already scaled to the tile (and filtered to the tile extent); not empty
template<class Backend>
void writePointFeature(
	typename Backend::LayerBuilder& vtLayer,
	bool includeID,
	const OutputObjectID& oo,
	const std::vector<std::pair<int, int>>& multipoint,
	const AttributeStore& attributeStore,
	unsigned zoom,
	MvtAttributeMemo* attrMemo
) {
	if constexpr (std::is_same<Backend, MvtBackend>::value) {
		if (attrMemo && attrMemo->usableFor(vtLayer, zoom)) {
			mvt_direct::Scratch& s = mvt_direct::scratch();
			mvt_direct::Geometry& g = s.geometry;
			g.clear();
			char* p = g.begin(1 + 2 * multipoint.size());
			p = mvt_direct::putVarint(p, vtzero::detail::command_move_to(multipoint.size()));
			for (const auto &point : multipoint) p = g.delta(p, point.first, point.second);
			g.end(p);
			uint32_t count;
			const uint32_t* kv = attrMemo->resolve(attributeStore, oo.oo.attributes, count);
			mvt_direct::commit(vtLayer, vtzero::GeomType::POINT, includeID && oo.id, oo.id, kv, count, s);
			return;
		}
	}
	typename Backend::PointFeature fbuilder{vtLayer};
	if (includeID && oo.id) fbuilder.set_id(oo.id);

	fbuilder.add_points(multipoint.size());

	for (const auto &point : multipoint)
		fbuilder.set_point(point.first, point.second);

	oo.oo.writeAttributes(attributeStore, fbuilder, zoom, attrMemo);
	fbuilder.commit();
}

// Linestrings in latp/lon (after simplification), scaled to the tile here
template<class Backend>
void writeLinestringFeature(
	typename Backend::LayerBuilder& vtLayer,
	bool includeID,
	const OutputObjectID& oo,
	const TileBbox& bbox,
	const MultiLinestring& toWrite,
	const AttributeStore& attributeStore,
	unsigned zoom,
	MvtAttributeMemo* attrMemo
) {
	if constexpr (std::is_same<Backend, MvtBackend>::value) {
		if (attrMemo && attrMemo->usableFor(vtLayer, zoom)) {
			mvt_direct::Scratch& s = mvt_direct::scratch();
			mvt_direct::Geometry& g = s.geometry;
			g.clear();
			bool hadLine = false;
			for (const Linestring& ls : toWrite) {
				if (ls.size() <= 1)
					continue;
				// scaled once, without zero-length segments
				std::vector<std::pair<int, int>>& pts = s.points;
				bbox.scaleLine(pts, ls);
				// A line has at least 2 points.
				if (pts.size() <= 1)
					continue;
				hadLine = true;
				char* p = g.begin(2 * pts.size() + 2);
				p = mvt_direct::putVarint(p, vtzero::detail::command_move_to(1));
				p = g.delta(p, pts[0].first, pts[0].second);
				p = mvt_direct::putVarint(p, vtzero::detail::command_line_to(uint32_t(pts.size() - 1)));
				for (std::size_t i = 1; i < pts.size(); i++) p = g.delta(p, pts[i].first, pts[i].second);
				g.end(p);
			}
			if (hadLine) {
				uint32_t count;
				const uint32_t* kv = attrMemo->resolve(attributeStore, oo.oo.attributes, count);
				mvt_direct::commit(vtLayer, vtzero::GeomType::LINESTRING, includeID && oo.id, oo.id, kv, count, s);
			}
			return;
		}
	}
	typename Backend::LinestringFeature fbuilder{vtLayer};

	if (includeID && oo.id)
		fbuilder.set_id(oo.id);

	bool hadLine = false;

	thread_local std::vector<std::pair<int, int>> pts;
	for (const Linestring& ls : toWrite) {
		if (ls.size() <= 1)
			continue;

		// vtzero dislikes linesegments that have zero-length segments,
		// e.g. where p(x) == p(x + 1). So filter those out.
		bbox.scaleLine(pts, ls);

		// A line has at least 2 points.
		if (pts.size() <= 1)
			continue;

		hadLine = true;
		fbuilder.add_linestring(int(pts.size()));
		for (const std::pair<int, int>& xy : pts)
			fbuilder.set_point(xy.first, xy.second);
	}

	if (hadLine) {
		// add the properties
		oo.oo.writeAttributes(attributeStore, fbuilder, zoom, attrMemo);
		// call commit() when you are done
		fbuilder.commit();
	}
}

template<class Backend>
bool writeRing(
	typename Backend::PolygonFeature& fbuilder,
	const Ring& ring,
	bool outer
) {
	// vtzero doesn't like zero-length segments in rings
	std::pair<int, int> lastXy = std::make_pair(0, 0);
	int points = 0;
	int64_t twiceArea = 0;
	for (const Point& point : ring) {
		std::pair<int, int> xy = std::make_pair(point.get<0>(), point.get<1>());
		if (points == 0 || xy != lastXy) {
			if (points > 0)
				twiceArea += int64_t(lastXy.first) * xy.second - int64_t(xy.first) * lastXy.second;
			points++;
			lastXy = xy;
		}
	}

	// A ring has at least 4 points, and on the integer grid it must not have
	// collapsed to a line (all points collinear).
	if (points <= 3 || twiceArea == 0)
		return false;

	bool firstPoint = true;
	Backend::addRing(fbuilder, points, outer);
	for (auto it = ring.rbegin(); it != ring.rend(); ++it) {
		const Point& point = *it;

		std::pair<int, int> xy =
			std::make_pair(point.get<0>(), point.get<1>());

		if (firstPoint || xy != lastXy) {
			firstPoint = false;
			lastXy = xy;
			fbuilder.set_point(xy.first, xy.second);
		}
	}

	return true;
}

// Polygons in tile coordinates (after simplification and repair)
template<class Backend>
void writePolygonFeature(
	typename Backend::LayerBuilder& vtLayer,
	bool includeID,
	const OutputObjectID& oo,
	const MultiPolygon& current,
	const AttributeStore& attributeStore,
	unsigned zoom,
	MvtAttributeMemo* attrMemo
) {
	if constexpr (std::is_same<Backend, MvtBackend>::value) {
		if (attrMemo && attrMemo->usableFor(vtLayer, zoom)) {
			mvt_direct::Scratch& s = mvt_direct::scratch();
			mvt_direct::Geometry& g = s.geometry;
			g.clear();
			// As writeRing: in reverse order, without zero-length segments (coordinates truncated
			// to int); a ring needs at least 4 points and must not have collapsed to a line (zero
			// area on the integer grid); the last point becomes ClosePath
			auto ring = [&](const Ring& r) -> bool {
				std::vector<std::pair<int, int>>& pts = s.points;
				pts.clear();
				int64_t twiceArea = 0;
				for (auto it = r.rbegin(); it != r.rend(); ++it) {
					const std::pair<int, int> xy(static_cast<int>(it->get<0>()), static_cast<int>(it->get<1>()));
					if (pts.empty() || xy != pts.back()) {
						if (!pts.empty())
							twiceArea += int64_t(pts.back().first) * xy.second - int64_t(xy.first) * pts.back().second;
						pts.push_back(xy);
					}
				}
				const std::size_t n = pts.size();
				if (n <= 3 || twiceArea == 0)
					return false;
				char* p = g.begin(2 * n + 3);
				p = mvt_direct::putVarint(p, vtzero::detail::command_move_to(1));
				p = g.delta(p, pts[0].first, pts[0].second);
				p = mvt_direct::putVarint(p, vtzero::detail::command_line_to(uint32_t(n - 2)));
				for (std::size_t i = 1; i + 1 < n; i++) p = g.delta(p, pts[i].first, pts[i].second);
				p = mvt_direct::putVarint(p, vtzero::detail::command_close_path());
				g.end(p);
				return true;
			};
			bool hadPoly = false;
			for (const auto& poly : current) {
				// If we failed to write the outer, no need to write the inners.
				if (!ring(poly.outer()))
					continue;
				hadPoly = true;
				for (const Ring& inner : poly.inners())
					ring(inner);
			}
			if (hadPoly) {
				uint32_t count;
				const uint32_t* kv = attrMemo->resolve(attributeStore, oo.oo.attributes, count);
				mvt_direct::commit(vtLayer, vtzero::GeomType::POLYGON, includeID && oo.id, oo.id, kv, count, s);
			}
			return;
		}
	}
	typename Backend::PolygonFeature fbuilder{vtLayer};

	if (includeID && oo.id)
		fbuilder.set_id(oo.id);

	bool hadPoly = false;

	for (const auto& p : current) {
		const Ring& ring = p.outer();

		// If we failed to write the outer, no need to write the inners.
		if (!writeRing<Backend>(fbuilder, ring, true))
			continue;

		hadPoly = true;

		for (const Ring& ring : p.inners())
			writeRing<Backend>(fbuilder, ring, false);
	}

	if (hadPoly) {
		// add the properties
		oo.oo.writeAttributes(attributeStore, fbuilder, zoom, attrMemo);
		// call commit() when you are done
		fbuilder.commit();
	}
}

// Writes the layers of layers.layerOrder into outputdata, in order. layerFn(group, vtLayer) fills
// the layer of layerOrder[group]; attrMemo is reset for each layer. data are the tile's objects
// per source. keepEmptyLayers: when merging into an existing tile.
template<class Layers, class LayerFn>
void encodeMvtTile(
	const Layers& layers,
	unsigned zoom,
	uint32_t extent,
	uint32_t mvtVersion,
	const std::vector<std::vector<OutputObjectID>>& data,
	bool keepEmptyLayers,
	const bool& stop,
	MvtAttributeMemo& attrMemo,
	std::string& outputdata,
	LayerFn&& layerFn
) {
	// Layers present in the tile's objects (each source is sorted by layer first)
	uint64_t present[4] = { 0, 0, 0, 0 };
	for (const auto& objects : data) {
		for (auto it = objects.begin(); it != objects.end(); ) {
			const unsigned layer = it->oo.layer;
			present[layer >> 6] |= uint64_t(1) << (layer & 63);
			it = std::upper_bound(it, objects.end(), layer, [](unsigned l, const OutputObjectID& o) { return l < o.oo.layer; });
		}
	}
	// Per layer group the bits of its layers, per zoom the bits of the layers shown at that zoom
	// (computed once per thread; the layer definitions don't change while tiles are written)
	struct LayerMasks {
		const void* layers = nullptr;
		std::size_t groups = 0, count = 0;
		std::vector<std::array<uint64_t, 4>> group;
		std::array<std::array<uint64_t, 4>, 32> zoom;
	};
	thread_local LayerMasks masks;
	if (masks.layers != &layers || masks.groups != layers.layerOrder.size() || masks.count != layers.layers.size()) {
		masks.layers = &layers;
		masks.groups = layers.layerOrder.size();
		masks.count = layers.layers.size();
		masks.group.assign(masks.groups, std::array<uint64_t, 4>{0, 0, 0, 0});
		for (auto& z : masks.zoom) z = std::array<uint64_t, 4>{0, 0, 0, 0};
		for (std::size_t g = 0; g < masks.groups; g++)
			for (const auto layerNum : layers.layerOrder[g])
				masks.group[g][(layerNum >> 6) & 3] |= uint64_t(1) << (layerNum & 63);
		for (std::size_t layerNum = 0; layerNum < masks.count; layerNum++)
			for (unsigned z = 0; z < 32; z++)
				if (z >= layers.layers[layerNum].minzoom && z <= layers.layers[layerNum].maxzoom)
					masks.zoom[z][(layerNum >> 6) & 3] |= uint64_t(1) << (layerNum & 63);
	}
	vtzero::tile_builder tile;
	for (std::size_t group = 0; group < layers.layerOrder.size(); group++) {
		if (stop) break;
		// A layer group without objects in its zoom range writes no features, and a layer
		// without features isn't serialized: skip it (unless merging into an existing tile)
		if (!keepEmptyLayers) {
			bool any = false;
			if (zoom < 32) {
				const auto& g = masks.group[group];
				const auto& z = masks.zoom[zoom];
				any = ((present[0] & g[0] & z[0]) | (present[1] & g[1] & z[1]) | (present[2] & g[2] & z[2]) | (present[3] & g[3] & z[3])) != 0;
			} else {
				for (const auto layerNum : layers.layerOrder[group]) {
					const auto& ld = layers.layers[layerNum];
					if (zoom >= ld.minzoom && zoom <= ld.maxzoom && (present[(layerNum >> 6) & 3] >> (layerNum & 63) & 1)) { any = true; break; }
				}
			}
			if (!any) continue;
		}
		const std::string& layerName = layers.layers[layers.layerOrder[group].at(0)].name;
		vtzero::layer_builder vtLayer{tile, layerName, mvtVersion, extent};
		attrMemo.reset(vtLayer, zoom);
		layerFn(group, vtLayer);
	}
	tile.serialize(outputdata);
}

#endif //_TILE_ENCODE_H
