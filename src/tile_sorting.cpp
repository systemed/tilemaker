#include <string>
#include <vector>
#include <iostream>
#include "tile_data_base.h"
#include "append_vector.h"
#include <boost/sort/sort.hpp>

template<typename OO> void sortOutputObjects(
    const unsigned int indexZoom,
    const size_t threadNum,
	typename AppendVectorNS::AppendVector<OO>::Iterator begin,
	typename AppendVectorNS::AppendVector<OO>::Iterator end
)
{
    // If the user is doing a a small extract, there are few populated
    // entries in `object`.
    //
    // e.g. Colorado has ~9 z6 tiles, 1 of which has 95% of its output
    // objects.
    //
    // This optimizes for the small extract case by doing:
    // - for each vector in objects
    //   - do a multi-threaded sort of vector
    //
    // For small extracts, this ensures that all threads are used even if
    // only a handful of entries in `objects` are non-empty.
    //
    // For a global extract, this will have some overhead of repeatedly
    // setting up/tearing down threads. In that case, it would be 
    // better to assign chunks of `objects` to each thread.
    //
    // That's a future performance improvement, so deferring for now.
    boost::sort::block_indirect_sort(
        begin, end, 
        [indexZoom](const OO& a, const OO& b) {
            // Cluster by parent zoom, so that a subsequent search
            // can find a contiguous range of entries for any tile
            // at zoom 6 or higher.
            const size_t aX = a.x;
            const size_t aY = a.y;
            const size_t bX = b.x;
            const size_t bY = b.y;
            for (size_t z = CLUSTER_ZOOM; z <= indexZoom; z++) {
                const auto aXz = aX / (1 << (indexZoom - z));
                const auto bXz = bX / (1 << (indexZoom - z));
                if (aXz != bXz)
                    return aXz < bXz;

                const auto aYz = aY / (1 << (indexZoom - z));
                const auto bYz = bY / (1 << (indexZoom - z));

                if (aYz != bYz)
                    return aYz < bYz;
            }

            return false;
        },
        threadNum
    );
}

template void sortOutputObjects<OutputObjectXY>(
    const unsigned int indexZoom,
    const size_t threadNum,
	typename AppendVectorNS::AppendVector<OutputObjectXY>::Iterator begin,
	typename AppendVectorNS::AppendVector<OutputObjectXY>::Iterator end
);

template void sortOutputObjects<OutputObjectXYID>(
    const unsigned int indexZoom,
    const size_t threadNum,
	typename AppendVectorNS::AppendVector<OutputObjectXYID>::Iterator begin,
	typename AppendVectorNS::AppendVector<OutputObjectXYID>::Iterator end
);

void sortOutputObjectIDs(
	const std::vector<bool>& sortOrders, 
	std::vector<OutputObjectID>& data
) {
	// Lexicographic comparison, with the order of: layer, geomType, attributes, and objectID.
	// Note that attributes is preferred to objectID.
	// It is to arrange objects with the identical attributes continuously.
	// Such objects will be merged into one object, to reduce the size of output.
	boost::sort::pdqsort(data.begin(), data.end(), [&sortOrders](const OutputObjectID& x, const OutputObjectID& y) -> bool {
		if (x.oo.layer < y.oo.layer) return true;
		if (x.oo.layer > y.oo.layer) return false;
		if (x.oo.z_order < y.oo.z_order) return  sortOrders[x.oo.layer];
		if (x.oo.z_order > y.oo.z_order) return !sortOrders[x.oo.layer];
		if (x.oo.geomType < y.oo.geomType) return true;
		if (x.oo.geomType > y.oo.geomType) return false;
		if (x.oo.attributes < y.oo.attributes) return true;
		if (x.oo.attributes > y.oo.attributes) return false;
		if (x.oo.objectID < y.oo.objectID) return true;
		return false;
	});
}

void sortTileCoordinates(
    const size_t baseZoom,
    const size_t threadNum,
    std::deque<std::pair<unsigned int, TileCoordinates>>& tileCoordinates
)
{
    boost::sort::block_indirect_sort(
		tileCoordinates.begin(), tileCoordinates.end(), 
		[baseZoom](auto const &a, auto const &b) {
			const auto aZoom = a.first;
			const auto bZoom = b.first;
			const auto aX = a.second.x;
			const auto aY = a.second.y;
			const auto bX = b.second.x;
			const auto bY = b.second.y;
			const bool aLowZoom = aZoom < CLUSTER_ZOOM;
			const bool bLowZoom = bZoom < CLUSTER_ZOOM;

			// Breadth-first for z0..5
			if (aLowZoom != bLowZoom)
				return aLowZoom;

			if (aLowZoom && bLowZoom) {
				if (aZoom != bZoom)
					return aZoom < bZoom;

				if (aX != bX)
					return aX < bX;

				return aY < bY;
			}

			for (size_t z = CLUSTER_ZOOM; z <= baseZoom; z++) {
				// Translate both a and b to zoom z, compare.
				// First, sanity check: can we translate it to this zoom?
				if (aZoom < z || bZoom < z) {
					return aZoom < bZoom;
				}

				const auto aXz = aX / (1 << (aZoom - z));
				const auto aYz = aY / (1 << (aZoom - z));
				const auto bXz = bX / (1 << (bZoom - z));
				const auto bYz = bY / (1 << (bZoom - z));

				if (aXz != bXz)
					return aXz < bXz;

				if (aYz != bYz)
					return aYz < bYz;
			}

			return false;
		}, 
		threadNum);
}