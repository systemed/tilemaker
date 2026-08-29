#include "declutter.h"

#include <algorithm>
#include <iostream>
#include <boost/geometry/index/rtree.hpp>

#include "shared_data.h"
#include "tile_data.h"

namespace bgi = boost::geometry::index;

// Features are ranked in world units (0-1 across the map in each direction), so a
// separation of n screen pixels at zoom z is simply n/(256<<z).
typedef boost::geometry::model::point<double, 2, boost::geometry::cs::cartesian> WorldPoint;

void Declutter::configure(const std::vector<LayerDef>& layers) {
	layerEnabled.assign(layers.size(), false);
	for (size_t i = 0; i < layers.size(); i++) {
		if (layers[i].declutterBelow == 0) continue;
		layerEnabled[i] = true;
		anyLayers = true;
	}
	if (!anyLayers) return;
	entries.resize(layers.size());
	mutexes = std::vector<std::mutex>(layers.size());
}

void Declutter::add(const OutputObject& oo, LatpLon point, uint64_t id, int32_t score, bool fromShapefile) {
	std::lock_guard<std::mutex> lock(mutexes[oo.layer]);
	entries[oo.layer].push_back({ oo, point, id, score, fromShapefile });
}

// Work up through the zoom levels, giving each feature the lowest zoom at which it both
// clears the score threshold and isn't crowded out by a feature already placed. The
// threshold halves at each zoom, and the separation is constant in screen terms, so each
// zoom admits both lower-scoring and more tightly packed features than the one before.
static void assignMinZooms(const LayerDef& layer, std::vector<DeclutterEntry>& list) {
	// Highest score first; ties broken on id and position so the ranking doesn't depend on
	// the order features happened to be read in
	std::sort(list.begin(), list.end(), [](const DeclutterEntry& a, const DeclutterEntry& b) {
		if (a.score != b.score) return a.score > b.score;
		if (a.id != b.id) return a.id < b.id;
		if (a.point.latp != b.point.latp) return a.point.latp < b.point.latp;
		return a.point.lon < b.point.lon;
	});

	bgi::rtree<WorldPoint, bgi::quadratic<16>> placed;
	std::vector<bool> done(list.size(), false);
	size_t remaining = list.size();

	double threshold = layer.declutterThreshold;
	for (uint z = layer.minzoom; z < layer.declutterBelow && remaining > 0; z++, threshold /= 2) {
		const double separation = layer.declutterDistance / (256.0 * (1u << z));

		for (size_t i = 0; i < list.size(); i++) {
			DeclutterEntry& e = list[i];
			if (e.score < threshold) break;		// sorted by descending score, so nothing further qualifies
			if (done[i] || e.oo.minZoom > z) continue;

			// Tweak for the Cheltenham case (two significant cities next to each other) - don't let it be repeatedly pushed out by Gloucester
			double allowed = separation;
			if (threshold > 0 && e.score > threshold * 5) allowed = separation / (e.score / threshold / 3.0);

			const double x = lon2tilexf(e.point.lon / 10000000.0, 0);
			const double y = latp2tileyf(e.point.latp / 10000000.0, 0);
			const WorldPoint p(x, y);

			// Anything within `allowed` of p is inside this box, so we only have to measure
			// the handful of features the box picks up
			const boost::geometry::model::box<WorldPoint> around(
				WorldPoint(x - allowed, y - allowed), WorldPoint(x + allowed, y + allowed));
			bool crowded = false;
			for (auto it = placed.qbegin(bgi::intersects(around)); it != placed.qend() && !crowded; ++it)
				crowded = boost::geometry::distance(p, *it) < allowed;
			if (crowded) continue;

			e.oo.setMinZoom(z);
			done[i] = true;
			remaining--;
			placed.insert(p);
		}
	}

	// Anything that never won a place appears from declutter_below upwards
	for (size_t i = 0; i < list.size(); i++)
		if (!done[i] && list[i].oo.minZoom < layer.declutterBelow)
			list[i].oo.setMinZoom(layer.declutterBelow);
}

void Declutter::apply(const std::vector<LayerDef>& layers, TileDataSource& osmSource, TileDataSource& shpSource) {
	if (!anyLayers) return;

	for (size_t i = 0; i < entries.size(); i++) {
		if (entries[i].empty()) continue;
		std::cout << "Decluttering " << entries[i].size() << " features in layer " << layers[i].name << ":" << std::flush;
		assignMinZooms(layers[i], entries[i]);

		std::vector<size_t> perZoom(16, 0);	// minZoom is a 4-bit field
		for (const auto& e : entries[i]) perZoom[e.oo.minZoom]++;
		for (size_t z = 0; z < perZoom.size(); z++)
			if (perZoom[z] > 0) std::cout << " z" << z << ":" << perZoom[z];
		std::cout << std::endl;

		for (const auto& e : entries[i]) {
			TileDataSource& source = e.fromShapefile ? shpSource : osmSource;
			source.addObjectToSmallIndex(latpLon2index(e.point, source.getIndexZoom()), e.oo, e.id);
		}
		std::vector<DeclutterEntry>().swap(entries[i]);
	}
}
