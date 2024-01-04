#pragma once

// Original source from https://github.com/mapbox/polylabel, licensed
// under ISC.
//
// Adapted to use Boost Geometry instead of MapBox's geometry library.
//
// Changes:
// - Default precision changed from 1 to 0.00001.
//   @mourner has some comments about what a reasonable precision value is
//   for latitude/longitude coordinates, see
//   https://github.com/mapbox/polylabel/issues/68#issuecomment-694906027
//   https://github.com/mapbox/polylabel/issues/103#issuecomment-1516623862
//
// Possible future changes:
// - Port the change described in https://github.com/mapbox/polylabel/issues/33,
//   implemented in Mapnik's Java renderer, to C++.
//   - But see counterexample: https://github.com/mapbox/polylabel/pull/63
//     @Fil also proposes an alternative approach there.
// - Pick precision as a function of the input geometry.

#include "geom.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <queue>

namespace mapbox {

namespace detail {

// get squared distance from a point to a segment
double getSegDistSq(const Point& p,
               const Point& a,
               const Point& b) {
    auto x = a.get<0>();
    auto y = a.get<1>();
    auto dx = b.get<0>() - x;
    auto dy = b.get<1>() - y;

    if (dx != 0 || dy != 0) {

        auto t = ((p.get<0>() - x) * dx + (p.get<1>() - y) * dy) / (dx * dx + dy * dy);

        if (t > 1) {
            x = b.get<0>();
            y = b.get<1>();

        } else if (t > 0) {
            x += dx * t;
            y += dy * t;
        }
    }

    dx = p.get<0>() - x;
    dy = p.get<1>() - y;

    return dx * dx + dy * dy;
}

// signed distance from point to polygon outline (negative if point is outside)
auto pointToPolygonDist(const Point& point, const Polygon& polygon) {
    bool inside = false;
    auto minDistSq = std::numeric_limits<double>::infinity();

    {
        const auto& ring = polygon.outer();
        for (std::size_t i = 0, len = ring.size(), j = len - 1; i < len; j = i++) {
            const auto& a = ring[i];
            const auto& b = ring[j];

            if ((a.get<1>() > point.get<1>()) != (b.get<1>() > point.get<1>()) &&
                (point.get<0>() < (b.get<0>() - a.get<0>()) * (point.get<1>() - a.get<1>()) / (b.get<1>() - a.get<1>()) + a.get<0>())) inside = !inside;

            minDistSq = std::min(minDistSq, getSegDistSq(point, a, b));
        }
    }

    for (const auto& ring : polygon.inners()) {
        for (std::size_t i = 0, len = ring.size(), j = len - 1; i < len; j = i++) {
            const auto& a = ring[i];
            const auto& b = ring[j];

            if ((a.get<1>() > point.get<1>()) != (b.get<1>() > point.get<1>()) &&
                (point.get<0>() < (b.get<0>() - a.get<0>()) * (point.get<1>() - a.get<1>()) / (b.get<1>() - a.get<1>()) + a.get<0>())) inside = !inside;

            minDistSq = std::min(minDistSq, getSegDistSq(point, a, b));
        }
    }

    return (inside ? 1 : -1) * std::sqrt(minDistSq);
}

struct Cell {
    Cell(const Point& c_, double h_, const Polygon& polygon)
        : c(c_),
          h(h_),
          d(pointToPolygonDist(c, polygon)),
          max(d + h * std::sqrt(2))
        {}

    Point c; // cell center
    double h; // half the cell size
    double d; // distance from cell center to polygon
    double max; // max distance to polygon within a cell
};

// get polygon centroid
Cell getCentroidCell(const Polygon& polygon) {
    double area = 0;
    double cx = 0, cy = 0;
    const auto& ring = polygon.outer();

    for (std::size_t i = 0, len = ring.size(), j = len - 1; i < len; j = i++) {
        const Point& a = ring[i];
        const Point& b = ring[j];
        auto f = a.get<0>() * b.get<1>() - b.get<0>() * a.get<1>();
        cx += (a.get<0>() + b.get<0>()) * f;
        cy += (a.get<1>() + b.get<1>()) * f;
        area += f * 3;
    }

    Point c { cx, cy };
    return Cell(area == 0 ? ring.at(0) : Point { cx / area, cy / area }, 0, polygon);
}

} // namespace detail

Point polylabel(const Polygon& polygon, double precision = 0.00001, bool debug = false) {
    using namespace detail;

    // find the bounding box of the outer ring
    Box envelope;
    geom::envelope(polygon.outer(), envelope);

    const Point size {
        envelope.max_corner().get<0>() - envelope.min_corner().get<0>(),
        envelope.max_corner().get<1>() - envelope.min_corner().get<1>()
    };

    const double cellSize = std::min(size.get<0>(), size.get<1>());
    double h = cellSize / 2;

    // a priority queue of cells in order of their "potential" (max distance to polygon)
    auto compareMax = [] (const Cell& a, const Cell& b) {
        return a.max < b.max;
    };
    using Queue = std::priority_queue<Cell, std::vector<Cell>, decltype(compareMax)>;
    Queue cellQueue(compareMax);

    if (cellSize == 0) {
        return envelope.min_corner();
    }

    // cover polygon with initial cells

    for (double x = envelope.min_corner().get<0>(); x < envelope.max_corner().get<0>(); x += cellSize) {
        for (double y = envelope.min_corner().get<1>(); y < envelope.max_corner().get<1>(); y += cellSize) {
            cellQueue.push(Cell({x + h, y + h}, h, polygon));
        }
    }

    // take centroid as the first best guess
    auto bestCell = getCentroidCell(polygon);

    // second guess: bounding box centroid
    Cell bboxCell(
        Point {
            envelope.min_corner().get<0>() + size.get<0>() / 2.0,
            envelope.min_corner().get<1>() + size.get<1>() / 2.0
        }, 0, polygon);
    if (bboxCell.d > bestCell.d) {
        bestCell = bboxCell;
    }

    auto numProbes = cellQueue.size();
    while (!cellQueue.empty()) {
        // pick the most promising cell from the queue
        auto cell = cellQueue.top();
        cellQueue.pop();

        // update the best cell if we found a better one
        if (cell.d > bestCell.d) {
            bestCell = cell;
            if (debug) std::cout << "found best " << ::round(1e4 * cell.d) / 1e4 << " after " << numProbes << " probes" << std::endl;
        }

        // do not drill down further if there's no chance of a better solution
        if (cell.max - bestCell.d <= precision) continue;

        // split the cell into four cells
        h = cell.h / 2;
        cellQueue.push(Cell({cell.c.get<0>() - h, cell.c.get<1>() - h}, h, polygon));
        cellQueue.push(Cell({cell.c.get<0>() + h, cell.c.get<1>() - h}, h, polygon));
        cellQueue.push(Cell({cell.c.get<0>() - h, cell.c.get<1>() + h}, h, polygon));
        cellQueue.push(Cell({cell.c.get<0>() + h, cell.c.get<1>() + h}, h, polygon));
        numProbes += 4;
    }

    if (debug) {
        std::cout << "num probes: " << numProbes << std::endl;
        std::cout << "best distance: " << bestCell.d << std::endl;
    }

    return bestCell.c;
}

} // namespace mapbox
