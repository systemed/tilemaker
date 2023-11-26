#include <algorithm>
#include <iostream>
#include "tile_data.h"

#include <ciso646>
#include <boost/sort/sort.hpp>

using namespace std;

typedef std::pair<OutputObjectsConstIt,OutputObjectsConstIt> OutputObjectsConstItPair;

// We cluster output objects by z6 tile
#define Z6_WIDTH (1 << 6)
#define Z6_AREA (Z6_WIDTH * Z6_WIDTH)

TileDataSource::TileDataSource(unsigned int baseZoom)
	: baseZoom(baseZoom),
	z6OffsetDivisor(baseZoom >= 6 ? (1 << (baseZoom - 6)) : 1)
{
	objects.resize(Z6_AREA);
	z6Offsets.resize(Z6_AREA);
}

void TileDataSource::addObjectToTileIndex(const TileCoordinates& index, const OutputObject& oo) {
	// TODO: shard this lock, we have 4096 vectors
	std::lock_guard<std::mutex> lock(mutex);

	// Pick the z6 index
	const size_t z6x = index.x / z6OffsetDivisor;
	const size_t z6y = index.y / z6OffsetDivisor;

	const size_t z6index = z6x * Z6_WIDTH + z6y;

	//std::cout << "adding to objects[" << z6index << "]: z" << baseZoom << " was: " << index.x << ", " << index.y << ", z6 was " << z6x << ", " << z6y << std::endl;
	objects[z6index].push_back(oo);
	z6Offsets[z6index].push_back(std::make_pair(
		index.x - (z6x * z6OffsetDivisor),
		index.y - (z6y * z6OffsetDivisor)
	));
}

void TileDataSource::collectTilesWithObjectsAtZoom(uint zoom, TileCoordinatesSet& output) {
	// Scan through all shards. Convert to base zoom, then convert to the requested zoom.

	for (size_t i = 0; i < objects.size(); i++) {
		const size_t z6x = i / Z6_WIDTH;
		const size_t z6y = i % Z6_WIDTH;

		for (size_t j = 0; j < objects[i].size(); j++) {
			// Compute the x, y at the base zoom level
			TileCoordinate baseX = z6x * z6OffsetDivisor + z6Offsets[i][j].first;
			TileCoordinate baseY = z6y * z6OffsetDivisor + z6Offsets[i][j].second;

			// Translate the x, y at the requested zoom level
			TileCoordinate x = baseX / (1 << (baseZoom - zoom));
			TileCoordinate y = baseY / (1 << (baseZoom - zoom));

			output.insert(TileCoordinates(x, y));
		}
	}
}

// Find the tiles used by the "large objects" from the rtree index
void TileDataSource::collectTilesWithLargeObjectsAtZoom(uint zoom, TileCoordinatesSet &output) {
	for(auto const &result: box_rtree) {
		int scale = pow(2, baseZoom-zoom);
		TileCoordinate minx = result.first.min_corner().x() / scale;
		TileCoordinate maxx = result.first.max_corner().x() / scale;
		TileCoordinate miny = result.first.min_corner().y() / scale;
		TileCoordinate maxy = result.first.max_corner().y() / scale;
		for (int x=minx; x<=maxx; x++) {
			for (int y=miny; y<=maxy; y++) {
				TileCoordinates newIndex(x, y);
				output.insert(newIndex);
			}
		}
	}
}

// Copy objects from the tile at dstIndex (in the dataset srcTiles) into output
void TileDataSource::collectObjectsForTile(
	uint zoom,
	TileCoordinates dstIndex,
	std::vector<OutputObject>& output
) {
	// TODO: this is brutally naive. Once we're at z1 or higher, we can skip
	//       certain shards that we know will never contribute.
	//
	//       At z6 or higher, we can pick the precise shard.

	size_t iStart = 0;
	size_t iEnd = objects.size();

	// TODO: we could narrow the search space for z1..z5, too.
	//       They're less important, as they have fewer tiles.
	if (zoom >= 6) {
		// Compute the x, y at the base zoom level
		TileCoordinate z6x = dstIndex.x / (1 << (zoom - 6));
		TileCoordinate z6y = dstIndex.y / (1 << (zoom - 6));

		iStart = z6x * Z6_WIDTH + z6y;
		iEnd = iStart + 1;
	}

	for (size_t i = iStart; i < iEnd; i++) {
		const size_t z6x = i / Z6_WIDTH;
		const size_t z6y = i % Z6_WIDTH;

		for (size_t j = 0; j < objects[i].size(); j++) {
			// Compute the x, y at the base zoom level
			TileCoordinate baseX = z6x * z6OffsetDivisor + z6Offsets[i][j].first;
			TileCoordinate baseY = z6y * z6OffsetDivisor + z6Offsets[i][j].second;

			// Translate the x, y at the requested zoom level
			TileCoordinate x = baseX / (1 << (baseZoom - zoom));
			TileCoordinate y = baseY / (1 << (baseZoom - zoom));

			if (dstIndex.x == x && dstIndex.y == y)
				output.push_back(objects[i][j]);
		}
	}
}

// Copy objects from the large index into output
void TileDataSource::collectLargeObjectsForTile(
	uint zoom,
	TileCoordinates dstIndex,
	std::vector<OutputObject>& output
) {
	int scale = pow(2, baseZoom - zoom);
	TileCoordinates srcIndex1( dstIndex.x   *scale  ,  dstIndex.y   *scale  );
	TileCoordinates srcIndex2((dstIndex.x+1)*scale-1, (dstIndex.y+1)*scale-1);
	Box box = Box(geom::make<Point>(srcIndex1.x, srcIndex1.y),
	              geom::make<Point>(srcIndex2.x, srcIndex2.y));
	for(auto const& result: box_rtree | boost::geometry::index::adaptors::queried(boost::geometry::index::intersects(box)))
		output.push_back(result.second);
}

// Build node and way geometries
Geometry TileDataSource::buildWayGeometry(OutputGeometryType const geomType, 
                                          NodeID const objectID, const TileBbox &bbox) const {
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
			if (!geom::is_valid(mp)) make_valid(mp);
			return mp;
		}

		default:
			throw std::runtime_error("Invalid output geometry");
	}
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
	std::cout << "Generated points: " << point_store->size() << ", lines: " << (linestring_store->size() + multi_linestring_store->size()) << ", polygons: " << multi_polygon_store->size() << std::endl;
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

std::vector<OutputObject> TileDataSource::getObjectsForTile(
	const std::vector<bool>& sortOrders, 
	unsigned int zoom,
	TileCoordinates coordinates
) {
	// TODO: once we cluster writes by z6, consider a thread-local
	//   cache of z6 tiles.
	//
	// In a perfect world, we'd share them so we only constructed the cache
	// once per z6 tile...but baby steps.
	std::vector<OutputObject> data;
	collectObjectsForTile(zoom, coordinates, data);
	collectLargeObjectsForTile(zoom, coordinates, data);

	// Lexicographic comparison, with the order of: layer, geomType, attributes, and objectID.
	// Note that attributes is preferred to objectID.
	// It is to arrange objects with the identical attributes continuously.
	// Such objects will be merged into one object, to reduce the size of output.
	boost::sort::pdqsort(data.begin(), data.end(), [&sortOrders](const OutputObject& x, const OutputObject& y) -> bool {
		if (x.layer < y.layer) return true;
		if (x.layer > y.layer) return false;
		if (x.z_order < y.z_order) return  sortOrders[x.layer];
		if (x.z_order > y.z_order) return !sortOrders[x.layer];
		if (x.geomType < y.geomType) return true;
		if (x.geomType > y.geomType) return false;
		if (x.attributes < y.attributes) return true;
		if (x.attributes > y.attributes) return false;
		if (x.objectID < y.objectID) return true;
		return false;
	});
	data.erase(unique(data.begin(), data.end()), data.end());
	return data;
}

OutputObjectsConstItPair getObjectsAtSubLayer(
	const std::vector<OutputObject>& data,
	uint_least8_t layerNum
) {
    struct layerComp
    {
        bool operator() ( const OutputObject& x, uint_least8_t layer ) const { return x.layer < layer; }
        bool operator() ( uint_least8_t layer, const OutputObject& x ) const { return layer < x.layer; }
    };

	// compare only by `layer`
	// We get the range within ooList, where the layer of each object is `layerNum`.
	// Note that ooList is sorted by a lexicographic order, `layer` being the most significant.
	return equal_range(data.begin(), data.end(), layerNum, layerComp());
}

// ------------------------------------
// Add geometries to tile/large indices

void TileDataSource::AddGeometryToIndex(
	const Linestring& geom,
	const std::vector<OutputObject>& outputs
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
				addObjectToTileIndex(index, output); // not a polygon
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
					AddObjectToLargeIndex(box, output);
				} else {
					// Smaller objects - add to each individual tile index
					if (!tilesetFilled) { fillCoveredTiles(tileSet); tilesetFilled = true; }
					for (auto it = tileSet.begin(); it != tileSet.end(); ++it) {
						TileCoordinates index = *it;
						addObjectToTileIndex(index, output);
					}
				}
			}
		}
	} catch(std::out_of_range &err) {
		cerr << "Error calculating intermediate tiles: " << err.what() << endl;
	}
}

void TileDataSource::AddGeometryToIndex(
	const MultiLinestring& geom,
	const std::vector<OutputObject>& outputs
) {
	for (Linestring ls : geom) {
		unordered_set<TileCoordinates> tileSet;
		insertIntermediateTiles(ls, baseZoom, tileSet);
		for (auto it = tileSet.begin(); it != tileSet.end(); ++it) {
			TileCoordinates index = *it;
			for (const auto& output : outputs) {
				addObjectToTileIndex(index, output);
			}
		}
	}
}

void TileDataSource::AddGeometryToIndex(
	const MultiPolygon& geom,
	const std::vector<OutputObject>& outputs
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
			AddObjectToLargeIndex(box, output);
		} else {
			// Smaller objects - add to each individual tile index
			for (auto it = tileSet.begin(); it != tileSet.end(); ++it) {
				TileCoordinates index = *it;
				addObjectToTileIndex(index, output);
			}
		}
	}
}
