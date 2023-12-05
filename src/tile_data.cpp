#include <algorithm>
#include <iostream>
#include "tile_data.h"

#include <bitset>
#include <ciso646>

using namespace std;
extern bool verbose;

TileDataSource::TileDataSource(size_t threadNum, unsigned int baseZoom, bool includeID)
	:
	includeID(includeID),
	z6OffsetDivisor(baseZoom >= CLUSTER_ZOOM ? (1 << (baseZoom - CLUSTER_ZOOM)) : 1),
	objectsMutex(threadNum * 4),
	objects(CLUSTER_ZOOM_AREA),
	objectsWithIds(CLUSTER_ZOOM_AREA),
	baseZoom(baseZoom),
	largeCoveringPolygons(threadNum * 4),
	largeExcludedPolygons(threadNum * 4)
{
}

void TileDataSource::finalize(size_t threadNum) {
	finalizeObjects<OutputObjectXY>(threadNum, baseZoom, objects.begin(), objects.end());
	finalizeObjects<OutputObjectXYID>(threadNum, baseZoom, objectsWithIds.begin(), objectsWithIds.end());
}

void TileDataSource::addObjectToSmallIndex(const TileCoordinates& index, const OutputObject& oo, uint64_t id) {
	// Pick the z6 index
	const size_t z6x = index.x / z6OffsetDivisor;
	const size_t z6y = index.y / z6OffsetDivisor;

	if (z6x >= 64 || z6y >= 64) {
		if (verbose) std::cerr << "ignoring OutputObject with invalid z" << baseZoom << " coordinates " << index.x << ", " << index.y << " (id: " << id << ")" << std::endl;
		return;
	}

	const size_t z6index = z6x * CLUSTER_ZOOM_WIDTH + z6y;

	std::lock_guard<std::mutex> lock(objectsMutex[z6index % objectsMutex.size()]);

	if (id == 0 || !includeID)
		objects[z6index].push_back({
			oo,
			(Z6Offset)(index.x - (z6x * z6OffsetDivisor)),
			(Z6Offset)(index.y - (z6y * z6OffsetDivisor))
		});
	else
		objectsWithIds[z6index].push_back({
			oo,
			(Z6Offset)(index.x - (z6x * z6OffsetDivisor)),
			(Z6Offset)(index.y - (z6y * z6OffsetDivisor)),
			id
		});
}

void TileDataSource::collectTilesWithObjectsAtZoom(uint zoom, TileCoordinatesSet& output) {
	// Scan through all shards. Convert to base zoom, then convert to the requested zoom.
	collectTilesWithObjectsAtZoomTemplate<OutputObjectXY>(baseZoom, objects.begin(), objects.size(), zoom, output);
	collectTilesWithObjectsAtZoomTemplate<OutputObjectXYID>(baseZoom, objectsWithIds.begin(), objectsWithIds.size(), zoom, output);
}

void addCoveredTilesToOutput(const uint baseZoom, const uint zoom, const Box& box, TileCoordinatesSet& output) {
	int scale = pow(2, baseZoom-zoom);
	TileCoordinate minx = box.min_corner().x() / scale;
	TileCoordinate maxx = box.max_corner().x() / scale;
	TileCoordinate miny = box.min_corner().y() / scale;
	TileCoordinate maxy = box.max_corner().y() / scale;
	for (int x=minx; x<=maxx; x++) {
		for (int y=miny; y<=maxy; y++) {
			TileCoordinates newIndex(x, y);
			output.insert(newIndex);
		}
	}
}

// Find the tiles used by the "large objects" from the rtree index
void TileDataSource::collectTilesWithLargeObjectsAtZoom(uint zoom, TileCoordinatesSet &output) {
	for(auto const &result: boxRtree)
		addCoveredTilesToOutput(baseZoom, zoom, result.first, output);

	for(auto const &result: boxRtreeWithIds)
		addCoveredTilesToOutput(baseZoom, zoom, result.first, output);
}

// Copy objects from the tile at dstIndex (in the dataset srcTiles) into output
void TileDataSource::collectObjectsForTile(
	uint zoom,
	TileCoordinates dstIndex,
	std::vector<OutputObjectID>& output
) {
	size_t iStart = 0;
	size_t iEnd = objects.size();

	// TODO: we could also narrow the search space for z1..z5, too.
	//       They're less important, as they have fewer tiles.
	if (zoom >= CLUSTER_ZOOM) {
		// Compute the x, y at the base zoom level
		TileCoordinate z6x = dstIndex.x / (1 << (zoom - CLUSTER_ZOOM));
		TileCoordinate z6y = dstIndex.y / (1 << (zoom - CLUSTER_ZOOM));

		iStart = z6x * CLUSTER_ZOOM_WIDTH + z6y;
		iEnd = iStart + 1;
	}

	collectObjectsForTileTemplate<OutputObjectXY>(baseZoom, objects.begin(), iStart, iEnd, zoom, dstIndex, output);
	collectObjectsForTileTemplate<OutputObjectXYID>(baseZoom, objectsWithIds.begin(), iStart, iEnd, zoom, dstIndex, output);
}

// Copy objects from the large index into output
void TileDataSource::collectLargeObjectsForTile(
	uint zoom,
	TileCoordinates dstIndex,
	std::vector<OutputObjectID>& output
) {
	int scale = pow(2, baseZoom - zoom);
	TileCoordinates srcIndex1( dstIndex.x   *scale  ,  dstIndex.y   *scale  );
	TileCoordinates srcIndex2((dstIndex.x+1)*scale-1, (dstIndex.y+1)*scale-1);
	Box box = Box(geom::make<Point>(srcIndex1.x, srcIndex1.y),
	              geom::make<Point>(srcIndex2.x, srcIndex2.y));
	for(auto const& result: boxRtree | boost::geometry::index::adaptors::queried(boost::geometry::index::intersects(box))) {
		if (result.second.minZoom <= zoom)
			output.push_back({result.second, 0});
	}

	for(auto const& result: boxRtreeWithIds | boost::geometry::index::adaptors::queried(boost::geometry::index::intersects(box))) {
		if (result.second.oo.minZoom <= zoom)
			output.push_back({result.second.oo, result.second.id});
	}
}

bool bboxInLargePolygonBitset(const TileBbox& bbox, uint64_t bits) {
	std::bitset<64> bitset(bits);

	const size_t baseXz6 = bbox.index.x / (1 << (bbox.zoom - 6));
	const size_t baseYz6 = bbox.index.y / (1 << (bbox.zoom - 6));

	// If zoom >= 9: can short-circuit if our z9 tile is set.
	if (bbox.zoom >= 9) {
		size_t z9x = bbox.index.x / (1 << (bbox.zoom - 9));
		size_t z9y = bbox.index.y / (1 << (bbox.zoom - 9));
		return bitset.test(z9x * 8 + z9y);
	}

	// If zoom is 7 or 8, it's a bit more involved -- we need our entire
	// range of z9 tiles to be set.
	if (bbox.zoom == 7 || bbox.zoom == 8) {
		const size_t stride = 1 << (9 - bbox.zoom);

		const size_t baseXz9 = baseXz6 * 8;
		const size_t baseYz9 = baseYz6 * 8;

		// Figure out where we'll start reading.
		const size_t startX = bbox.index.x * (1 << (9 - bbox.zoom)) - baseXz9;
		const size_t startY = bbox.index.y * (1 << (9 - bbox.zoom)) - baseYz9;

		bool success = true;
		for (int x = startX; x < startX + stride; x++) {
			for (int y = startY; y < startY + stride; y++) {
				success = success && bitset.test(x * 8 + y);
			}
		}
		return success;
	}

	// You shouldn't call this for zooms <= 6.
	return false;
}

// Build node and way geometries
Geometry TileDataSource::buildWayGeometry(OutputGeometryType const geomType, 
                                          NodeID const objectID, const TileBbox &bbox) {
	switch(geomType) {
		case POINT_: {
			auto p = retrieve_point(objectID);
			if (geom::within(p, bbox.clippingBox)) {
				return p;
			} 
			return MultiLinestring();
		}

		case LINESTRING_: {
			auto const &ls = retrieve_linestring(objectID);

			MultiLinestring out;
			if(ls.empty())
				return out;

			Linestring current_ls;
			geom::append(current_ls, ls[0]);

			for(size_t i = 1; i < ls.size(); ++i) {
				if(!geom::intersects(Linestring({ ls[i-1], ls[i] }), bbox.clippingBox)) {
					if(current_ls.size() > 1)
						out.push_back(std::move(current_ls));
					current_ls.clear();
				}
				geom::append(current_ls, ls[i]);
			}

			if(current_ls.size() > 1)
				out.push_back(std::move(current_ls));

			MultiLinestring result;
			geom::intersection(out, bbox.getExtendBox(), result);
			return result;
		}

		case MULTILINESTRING_: {
			auto const &mls = retrieve_multi_linestring(objectID);
			// investigate whether filtering the constituent linestrings improves performance
			MultiLinestring result;
			geom::intersection(mls, bbox.getExtendBox(), result);
			return result;
		}

		case POLYGON_: {
			if (bbox.zoom > 6) {
				// If we're at a high enough zoom, see if we can short-circuit clipping.
				//
				// This is possible for very large polygons that cover (or fail to cover)
				// an entire z6/z7/z8/z9 tile.

				const size_t baseXz6 = bbox.index.x / (1 << (bbox.zoom - 6));
				const size_t baseYz6 = bbox.index.y / (1 << (bbox.zoom - 6));
				const uint16_t z6Tile = baseXz6 * 64 + baseYz6;

				const uint16_t coveringShard = objectID % largeCoveringPolygons.size();
				const uint16_t excludedShard = objectID % largeExcludedPolygons.size();
				const uint16_t lockShard = objectID % objectsMutex.size();
				uint64_t coveringBits = 0;
				uint64_t excludedBits = 0;
				{
					std::lock_guard<std::mutex> lock(objectsMutex[lockShard]);
					{
						const auto& rv = largeCoveringPolygons[coveringShard].find(std::make_pair(z6Tile, objectID));
						if (rv != largeCoveringPolygons[coveringShard].end()) {
							coveringBits = rv->second;
						}
					}

					const auto& rv = largeExcludedPolygons[excludedShard].find(std::make_pair(z6Tile, objectID));
					if (rv != largeExcludedPolygons[excludedShard].end()) {
						excludedBits = rv->second;
					}
				}

				if (coveringBits != 0 && bboxInLargePolygonBitset(bbox, coveringBits)) {
					MultiPolygon mp;
					Polygon p;
					boost::geometry::assign(p, bbox.clippingBox);
					mp.push_back(p);
					return mp;
				}

				if (excludedBits != 0 && bboxInLargePolygonBitset(bbox, excludedBits)) {
					MultiPolygon mp;
					return mp;
				}

			}

			auto const &input = retrieve_multi_polygon(objectID);

			Box box = bbox.clippingBox;
			
			if (bbox.endZoom) {
				for(auto const &p: input) {
					for(auto const &inner: p.inners()) {
						for(std::size_t i = 0; i < inner.size() - 1; ++i) 
						{
							Point p1 = inner[i];
							Point p2 = inner[i + 1];

							if(geom::within(p1, bbox.clippingBox) != geom::within(p2, bbox.clippingBox)) {
								box.min_corner() = Point(	
									std::min(box.min_corner().x(), std::min(p1.x(), p2.x())), 
									std::min(box.min_corner().y(), std::min(p1.y(), p2.y())));
								box.max_corner() = Point(	
									std::max(box.max_corner().x(), std::max(p1.x(), p2.x())), 
									std::max(box.max_corner().y(), std::max(p1.y(), p2.y())));
							}
						}
					}

					for(std::size_t i = 0; i < p.outer().size() - 1; ++i) {
						Point p1 = p.outer()[i];
						Point p2 = p.outer()[i + 1];

						if(geom::within(p1, bbox.clippingBox) != geom::within(p2, bbox.clippingBox)) {
							box.min_corner() = Point(	
								std::min(box.min_corner().x(), std::min(p1.x(), p2.x())), 
								std::min(box.min_corner().y(), std::min(p1.y(), p2.y())));
							box.max_corner() = Point(	
								std::max(box.max_corner().x(), std::max(p1.x(), p2.x())), 
								std::max(box.max_corner().y(), std::max(p1.y(), p2.y())));
						}
					}
				}

				Box extBox = bbox.getExtendBox();
				box.min_corner() = Point(	
					std::max(box.min_corner().x(), extBox.min_corner().x()), 
					std::max(box.min_corner().y(), extBox.min_corner().y()));
				box.max_corner() = Point(	
					std::min(box.max_corner().x(), extBox.max_corner().x()), 
					std::min(box.max_corner().y(), extBox.max_corner().y()));
			}

			MultiPolygon mp;
			geom::assign(mp, input);
			fast_clip(mp, box);
			geom::correct(mp);
			geom::validity_failure_type failure = geom::validity_failure_type::no_failure;
			if (!geom::is_valid(mp,failure)) { 
				if (failure==geom::failure_spikes) {
					geom::remove_spikes(mp);
				} else if (failure==geom::failure_self_intersections || failure==geom::failure_intersecting_interiors) {
					// retry with Boost intersection if fast_clip has caused self-intersections
					MultiPolygon output;
					geom::intersection(input, box, output);
					geom::correct(output);

					updateLargePolygonMaps(objectID, bbox, mp);
					return output;
				} else {
					// occasionally also wrong_topological_dimension, disconnected_interior
				}
			}

			updateLargePolygonMaps(objectID, bbox, mp);
			return mp;
		}

		default:
			throw std::runtime_error("Invalid output geometry");
	}
}

void TileDataSource::updateLargePolygonMaps(NodeID objectID, const TileBbox& bbox, const MultiPolygon& mp) {
	// Did this polygon clip to the full extent of the tile, or clip to nothing?
	//
	// If yes, flag it so future tiles at higher zooms can avoid doing an
	// expensive clip.
	if (bbox.zoom < 6 || bbox.zoom > 9)
		return;

	bool isExcluded = false;
	bool isCovered = false;

	if (mp.size() == 0) {
		isExcluded = true;
	} else if (mp.size() == 1 && mp[0].outer().size() == 5) {
		Polygon bboxAsPolygon;
		boost::geometry::assign(bboxAsPolygon, bbox.clippingBox);

		if (boost::geometry::equals(mp[0], bboxAsPolygon))
			isCovered = true;
	}

	if (!isExcluded && !isCovered)
		return;

	// Truncate to the z6 x/y tile
	const size_t baseXz6 = bbox.index.x / (1 << (bbox.zoom - 6));
	const size_t baseYz6 = bbox.index.y / (1 << (bbox.zoom - 6));

	const uint16_t z6Tile = baseXz6 * 64 + baseYz6;

	auto& store = isCovered ? largeCoveringPolygons : largeExcludedPolygons;
	const uint16_t shard = objectID % store.size();
	const uint16_t lockShard = objectID % objectsMutex.size();

	uint64_t bits = 0;
	const std::pair<uint16_t, NodeID> key = std::make_pair(z6Tile, objectID);
	std::lock_guard<std::mutex> lock(objectsMutex[lockShard]);
	const auto& rv = store[shard].find(key);
	if (rv != store[shard].end())
		bits = rv->second;

	std::bitset<64> covered(bits);

	// We're at zoom 6, 7, 8, or 9, so we'll cover an 8x8, 4x4, 2x2 or 1x1 area
	// of z9 tiles.
	const size_t stride = 1 << (9 - bbox.zoom);

	const size_t baseXz9 = baseXz6 * 8;
	const size_t baseYz9 = baseYz6 * 8;

	// Figure out where we'll start writing.
	const size_t startX = bbox.index.x * (1 << (9 - bbox.zoom)) - baseXz9;
	const size_t startY = bbox.index.y * (1 << (9 - bbox.zoom)) - baseYz9;

	for (int x = startX; x < startX + stride; x++) {
		for (int y = startY; y < startY + stride; y++) {
			covered.set(x * 8 + y);
		}
	}

	store[shard][key] = covered.to_ullong();
}

LatpLon TileDataSource::buildNodeGeometry(OutputGeometryType const geomType, 
                                          NodeID const objectID, const TileBbox &bbox) const {
	switch(geomType) {
		case POINT_: {
			auto p = retrieve_point(objectID);
			LatpLon out;
			out.latp = p.y();
			out.lon  = p.x();
			return out;
		}

		default:
			break;
	}

	throw std::runtime_error("Geometry type is not point");			
}


// Report number of stored geometries
void TileDataSource::reportSize() const {
	std::cout << "Generated points: " << (point_store->size()-1) << ", lines: " << (linestring_store->size() + multi_linestring_store->size() - 2) << ", polygons: " << (multi_polygon_store->size()-1) << std::endl;
}

TileCoordinatesSet getTilesAtZoom(
	const std::vector<class TileDataSource *>& sources,
	unsigned int zoom
) {
	TileCoordinatesSet tileCoordinates;

	// Create list of tiles
	tileCoordinates.clear();
	for(size_t i=0; i<sources.size(); i++) {
		sources[i]->collectTilesWithObjectsAtZoom(zoom, tileCoordinates);
		sources[i]->collectTilesWithLargeObjectsAtZoom(zoom, tileCoordinates);
	}

	return tileCoordinates;
}

std::vector<OutputObjectID> TileDataSource::getObjectsForTile(
	const std::vector<bool>& sortOrders, 
	unsigned int zoom,
	TileCoordinates coordinates
) {
	std::vector<OutputObjectID> data;
	collectObjectsForTile(zoom, coordinates, data);
	collectLargeObjectsForTile(zoom, coordinates, data);

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
	data.erase(unique(data.begin(), data.end()), data.end());
	return data;
}

// ------------------------------------
// Add geometries to tile/large indices

void TileDataSource::addGeometryToIndex(
	const Linestring& geom,
	const std::vector<OutputObject>& outputs,
	const uint64_t id
) {
	unordered_set<TileCoordinates> tileSet;
	try {
		insertIntermediateTiles(geom, baseZoom, tileSet);

		bool polygonExists = false;
		TileCoordinate minTileX = TILE_COORDINATE_MAX, maxTileX = 0, minTileY = TILE_COORDINATE_MAX, maxTileY = 0;
		for (auto it = tileSet.begin(); it != tileSet.end(); ++it) {
			TileCoordinates index = *it;
			minTileX = std::min(index.x, minTileX);
			minTileY = std::min(index.y, minTileY);
			maxTileX = std::max(index.x, maxTileX);
			maxTileY = std::max(index.y, maxTileY);
			for (const auto& output : outputs) {
				if (output.geomType == POLYGON_) {
					polygonExists = true;
					continue;
				}
				addObjectToSmallIndex(index, output, id); // not a polygon
			}
		}

		// for polygon, fill inner tiles
		if (polygonExists) {
			bool tilesetFilled = false;
			uint size = (maxTileX - minTileX + 1) * (maxTileY - minTileY + 1);
			for (const auto& output : outputs) {
				if (output.geomType != POLYGON_) continue;
				if (size>= 16) {
					// Larger objects - add to rtree
					Box box = Box(geom::make<Point>(minTileX, minTileY),
					              geom::make<Point>(maxTileX, maxTileY));
					addObjectToLargeIndex(box, output, id);
				} else {
					// Smaller objects - add to each individual tile index
					if (!tilesetFilled) { fillCoveredTiles(tileSet); tilesetFilled = true; }
					for (auto it = tileSet.begin(); it != tileSet.end(); ++it) {
						TileCoordinates index = *it;
						addObjectToSmallIndex(index, output, id);
					}
				}
			}
		}
	} catch(std::out_of_range &err) {
		cerr << "Error calculating intermediate tiles: " << err.what() << endl;
	}
}

void TileDataSource::addGeometryToIndex(
	const MultiLinestring& geom,
	const std::vector<OutputObject>& outputs,
	const uint64_t id
) {
	for (Linestring ls : geom) {
		unordered_set<TileCoordinates> tileSet;
		insertIntermediateTiles(ls, baseZoom, tileSet);
		for (auto it = tileSet.begin(); it != tileSet.end(); ++it) {
			TileCoordinates index = *it;
			for (const auto& output : outputs) {
				addObjectToSmallIndex(index, output, id);
			}
		}
	}
}

void TileDataSource::addGeometryToIndex(
	const MultiPolygon& geom,
	const std::vector<OutputObject>& outputs,
	const uint64_t id
) {
	unordered_set<TileCoordinates> tileSet;
	bool singleOuter = geom.size()==1;
	for (Polygon poly : geom) {
		unordered_set<TileCoordinates> tileSetTmp;
		insertIntermediateTiles(poly.outer(), baseZoom, tileSetTmp);
		fillCoveredTiles(tileSetTmp);
		if (singleOuter) {
			tileSet = std::move(tileSetTmp);
		} else {
			tileSet.insert(tileSetTmp.begin(), tileSetTmp.end());
		}
	}

	TileCoordinate minTileX = TILE_COORDINATE_MAX, maxTileX = 0, minTileY = TILE_COORDINATE_MAX, maxTileY = 0;
	for (auto it = tileSet.begin(); it != tileSet.end(); ++it) {
		TileCoordinates index = *it;
		minTileX = std::min(index.x, minTileX);
		minTileY = std::min(index.y, minTileY);
		maxTileX = std::max(index.x, maxTileX);
		maxTileY = std::max(index.y, maxTileY);
	}
	for (const auto& output : outputs) {
		if (tileSet.size()>=16) {
			// Larger objects - add to rtree
			// note that the bbox is currently the envelope of the entire multipolygon,
			// which is suboptimal in shapes like (_) ...... (_) where the outers are significantly disjoint
			Box box = Box(geom::make<Point>(minTileX, minTileY),
			              geom::make<Point>(maxTileX, maxTileY));
			addObjectToLargeIndex(box, output, id);
		} else {
			// Smaller objects - add to each individual tile index
			for (auto it = tileSet.begin(); it != tileSet.end(); ++it) {
				TileCoordinates index = *it;
				addObjectToSmallIndex(index, output, id);
			}
		}
	}
}
