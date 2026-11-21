/*! \file */
#include "cluster.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <unordered_map>

namespace {

// Buckets points into square cells the size of the clustering distance, so
// anything within that distance of a point lies in its own or a neighbouring cell
class PointGrid {
public:
	explicit PointGrid(double cellSize) : cellSize(cellSize) { }

	void add(const std::pair<int,int> &p, uint32_t value) {
		cells[key(cell(p.first), cell(p.second))].push_back(value);
	}

	// How many points are in this point's cell and those around it
	size_t countNear(const std::pair<int,int> &p) const {
		size_t count = 0;
		forEachCellNear(p, [&](const std::vector<uint32_t> &values) { count += values.size(); });
		return count;
	}

	template<class F>
	void forEachNear(const std::pair<int,int> &p, F&& fn) const {
		forEachCellNear(p, [&](const std::vector<uint32_t> &values) {
			for (uint32_t value : values) fn(value);
		});
	}

private:
	double cellSize;
	std::unordered_map<uint64_t, std::vector<uint32_t>> cells;

	template<class F>
	void forEachCellNear(const std::pair<int,int> &p, F&& fn) const {
		const int64_t cx = cell(p.first), cy = cell(p.second);
		for (int64_t x = cx-1; x <= cx+1; x++) {
			for (int64_t y = cy-1; y <= cy+1; y++) {
				auto it = cells.find(key(x, y));
				if (it != cells.end()) fn(it->second);
			}
		}
	}

	int64_t cell(int v) const { return static_cast<int64_t>(std::floor(v / cellSize)); }
	static uint64_t key(int64_t x, int64_t y) {
		return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) | static_cast<uint32_t>(y);
	}
};

inline double distanceSquared(const std::pair<int,int> &a, const std::pair<int,int> &b) {
	const double dx = a.first - b.first, dy = a.second - b.second;
	return dx*dx + dy*dy;
}

}

std::vector<size_t> clusterPoints(const std::vector<std::pair<int,int>> &points, double distance) {
	const size_t n = points.size();
	std::vector<size_t> result;
	if (n <= 1 || distance <= 0) {
		result.resize(n);
		std::iota(result.begin(), result.end(), 0);
		return result;
	}
	const double maxSquared = distance * distance;

	// Estimate the density around each point, so that clusters are seeded from the
	// densest points first and centre on the busiest part of a group
	PointGrid pointGrid(distance);
	for (size_t i = 0; i < n; i++) pointGrid.add(points[i], i);
	std::vector<size_t> neighbours(n);
	for (size_t i = 0; i < n; i++) neighbours[i] = pointGrid.countNear(points[i]);
	std::vector<uint32_t> order(n);
	std::iota(order.begin(), order.end(), 0);
	std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
		if (neighbours[a] != neighbours[b]) return neighbours[a] > neighbours[b];
		if (points[a] != points[b]) return points[a] < points[b];
		return a < b;
	});

	// A point becomes a seed unless an existing seed is within the distance
	std::vector<uint32_t> seeds;
	PointGrid seedGrid(distance);
	auto nearestSeed = [&](const std::pair<int,int> &p) -> int64_t {
		int64_t best = -1;
		double bestSquared = maxSquared;
		seedGrid.forEachNear(p, [&](uint32_t s) {
			const double d = distanceSquared(p, points[seeds[s]]);
			if (d < bestSquared || (d == bestSquared && (best < 0 || s < best))) {
				best = s;
				bestSquared = d;
			}
		});
		return best;
	};
	for (uint32_t i : order) {
		if (nearestSeed(points[i]) >= 0) continue;
		seedGrid.add(points[i], seeds.size());
		seeds.push_back(i);
	}

	// Every point joins its nearest seed (there is always one within the distance)
	std::vector<uint32_t> membership(n);
	std::vector<double> sumX(seeds.size(), 0), sumY(seeds.size(), 0);
	std::vector<uint32_t> count(seeds.size(), 0);
	for (size_t i = 0; i < n; i++) {
		const uint32_t s = static_cast<uint32_t>(nearestSeed(points[i]));
		membership[i] = s;
		sumX[s] += points[i].first;
		sumY[s] += points[i].second;
		count[s]++;
	}

	// Represent each cluster by its member nearest the centroid, so the point
	// written is a real location rather than an average of several
	std::vector<int64_t> best(seeds.size(), -1);
	std::vector<double> bestSquared(seeds.size(), 0);
	for (size_t i = 0; i < n; i++) {
		const uint32_t s = membership[i];
		const double dx = points[i].first  - sumX[s] / count[s];
		const double dy = points[i].second - sumY[s] / count[s];
		const double d = dx*dx + dy*dy;
		if (best[s] < 0 || d < bestSquared[s] || (d == bestSquared[s] && points[i] < points[best[s]])) {
			best[s] = i;
			bestSquared[s] = d;
		}
	}
	for (int64_t i : best) result.push_back(static_cast<size_t>(i));
	std::sort(result.begin(), result.end());
	return result;
}

void clusterAttributeKey(const AttributeStore &attributeStore, AttributeIndex attributes, unsigned zoom, std::string &key) {
	std::vector<std::pair<uint16_t, const AttributePair*>> visible;
	attributeStore.forEachPairUnsafe(attributes, [&](const AttributePair &pair) {
		if (pair.minzoom <= zoom) visible.emplace_back(pair.keyIndex, &pair);
	});
	std::sort(visible.begin(), visible.end(), [](const auto &a, const auto &b) { return a.first < b.first; });

	key.clear();
	auto append = [&](const void *data, size_t size) { key.append(static_cast<const char*>(data), size); };
	for (const auto &entry : visible) {
		const AttributePair &pair = *entry.second;
		const uint8_t type = static_cast<uint8_t>(pair.valueType);
		append(&entry.first, sizeof(entry.first));
		append(&type, sizeof(type));
		if (pair.hasStringValue()) {
			const PooledString &s = pair.pooledString();
			const uint32_t size = s.size();
			append(&size, sizeof(size));
			append(s.data(), size);
		} else if (pair.hasBoolValue()) {
			const bool v = pair.boolValue();
			append(&v, sizeof(v));
		} else if (pair.hasIntValue()) {
			const int v = pair.intValue();
			append(&v, sizeof(v));
		} else {
			const float v = pair.floatValue();
			append(&v, sizeof(v));
		}
	}
}
