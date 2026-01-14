// This file contains sorting implementations that use boost::sort.
// boost::sort conflicts with boost::geometry's boost::range::sort in Boost 1.89+.
// Files that need both should use std::sort instead.
// This file needs boost::geometry (via tile_data_base.h) for struct definitions,
// so we use std::sort here as well for Boost 1.89+ compatibility.

#include "tile_data_base.h"
#include "append_vector.h"
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

template<typename OO> void finalizeObjects(
	const std::string& name,
	const size_t& threadNum,
	const unsigned int& indexZoom,
	typename std::vector<AppendVectorNS::AppendVector<OO>>::iterator begin,
	typename std::vector<AppendVectorNS::AppendVector<OO>>::iterator end,
	typename std::vector<std::vector<OO>>& lowZoom
	) {
	size_t z6OffsetDivisor = indexZoom >= CLUSTER_ZOOM ? (1 << (indexZoom - CLUSTER_ZOOM)) : 1;
#ifdef CLOCK_MONOTONIC
	timespec startTs, endTs;
	clock_gettime(CLOCK_MONOTONIC, &startTs);
#endif

	int i = -1;
	for (auto it = begin; it != end; it++) {
		i++;
		if (it->size() > 0 || i % 50 == 0 || i == 4095) {
			std::cout << "\r" << name << ": finalizing z6 tile " << (i + 1) << "/" << CLUSTER_ZOOM_AREA;

#ifdef CLOCK_MONOTONIC
			clock_gettime(CLOCK_MONOTONIC, &endTs);
			uint64_t elapsedNs = 1e9 * (endTs.tv_sec - startTs.tv_sec) + endTs.tv_nsec - startTs.tv_nsec;
			std::cout << " (" << std::to_string((uint32_t)(elapsedNs / 1e6)) << " ms)";
#endif
			std::cout << std::flush;
		}
		if (it->size() == 0)
			continue;

		// We track a separate copy of low zoom objects to avoid scanning large
		// lists of objects that may be on slow disk storage.
		for (auto objectIt = it->begin(); objectIt != it->end(); objectIt++)
			if (objectIt->oo.minZoom < CLUSTER_ZOOM)
				lowZoom[i].push_back(*objectIt);

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
		// Note: Using std::sort for Boost 1.89+ compatibility (boost::sort conflicts with boost::geometry)
		std::sort(
			it->begin(),
			it->end(),
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
			}
		);
	}

	std::cout << std::endl;
}

// Explicit template instantiations
template void finalizeObjects<OutputObjectXY>(
	const std::string& name,
	const size_t& threadNum,
	const unsigned int& indexZoom,
	std::vector<AppendVectorNS::AppendVector<OutputObjectXY>>::iterator begin,
	std::vector<AppendVectorNS::AppendVector<OutputObjectXY>>::iterator end,
	std::vector<std::vector<OutputObjectXY>>& lowZoom
);

template void finalizeObjects<OutputObjectXYID>(
	const std::string& name,
	const size_t& threadNum,
	const unsigned int& indexZoom,
	std::vector<AppendVectorNS::AppendVector<OutputObjectXYID>>::iterator begin,
	std::vector<AppendVectorNS::AppendVector<OutputObjectXYID>>::iterator end,
	std::vector<std::vector<OutputObjectXYID>>& lowZoom
);
