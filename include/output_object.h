/*! \file */ 
#ifndef _OUTPUT_OBJECT_H
#define _OUTPUT_OBJECT_H

#include <vector>
#include <string>
#include <map>
#include <memory>
#include "geom.h"
#include "coordinates.h"
#include "attribute_store.h"
#include "osm_store.h"

// Protobuf
#include "osmformat.pb.h"
#include "vector_tile.pb.h"

#include <boost/intrusive_ptr.hpp>
#include <atomic>

enum OutputGeometryType { POINT_, LINESTRING_, POLYGON_ };

//\brief Display the geometry type
std::ostream& operator<<(std::ostream& os, OutputGeometryType geomType);

/**
 * \brief OutputObject - any object (node, linestring, polygon) to be outputted to tiles

 * Possible future improvements to save memory:
 * - use a global dictionary for attribute key/values
*/
class OutputObject {

protected:	
	OutputObject(OutputGeometryType type, bool shp, uint_least8_t l, NodeID id, OSMStore::handle_t handle, AttributeStoreRef attributes) 
		: objectID(id), handle(handle), geomType(type), fromShapefile(shp), layer(l), z_order(0),
		  minZoom(0), references(0), attributes(attributes)
	{ }


public:
	NodeID objectID 			: 40;					// id of way (linestring/polygon) or node (point)
	uint_least8_t layer 		: 8;					// what layer is it in?
	int8_t z_order				: 8;					// z_order: used for sorting features within layers
	OutputGeometryType geomType : 2;					// point, linestring, polygon
	bool fromShapefile 			: 1;
	unsigned minZoom 			: 4;

	OSMStore::handle_t handle;							// Handle within global store of geometries
	mutable std::atomic<uint32_t> references;
	AttributeStoreRef attributes;

	void setZOrder(const int z) {
		if (z <= -127 || z >= 127) {
			throw std::runtime_error("z_order is limited to 1 byte signed integer.");
		}
		z_order = z;
	}

	void setMinZoom(unsigned z) {
		minZoom = z;
	}

	void setAttributeSet(AttributeStoreRef attributes) {
		this->attributes = attributes;
	}

	//\brief Write attribute key/value pairs (dictionary-encoded)
	void writeAttributes(std::vector<std::string> *keyList, 
		std::vector<vector_tile::Tile_Value> *valueList, vector_tile::Tile_Feature *featurePtr, char zoom) const;
	
	/**
	 * \brief Find a value in the value dictionary
	 * (we can't easily use find() because of the different value-type encoding - 
	 *	should be possible to improve this though)
	 */
	int findValue(std::vector<vector_tile::Tile_Value> *valueList, vector_tile::Tile_Value const &value) const;
};

/**
 * \brief An OutputObject derived class that contains data originally from OsmMemTiles
*/
class OutputObjectOsmStorePoint : public OutputObject
{
public:
	OutputObjectOsmStorePoint(OutputGeometryType type, bool shp, uint_least8_t l, NodeID id, OSMStore::handle_t handle, AttributeStoreRef attributes)
		: OutputObject(type, shp, l, id, handle, attributes)
	{ 
		assert(type == POINT_);
	}
}; 

class OutputObjectOsmStoreLinestring : public OutputObject
{
public:
	OutputObjectOsmStoreLinestring(OutputGeometryType type, bool shp, uint_least8_t l, NodeID id, OSMStore::handle_t handle, AttributeStoreRef attributes)
		: OutputObject(type, shp, l, id, handle, attributes)
	{ 
		assert(type == LINESTRING_);
	}
};

class OutputObjectOsmStoreMultiPolygon : public OutputObject
{
public:
	OutputObjectOsmStoreMultiPolygon(OutputGeometryType type, bool shp, uint_least8_t l, NodeID id, OSMStore::handle_t handle, AttributeStoreRef attributes)
		: OutputObject(type, shp, l, id, handle, attributes)
	{ 
		assert(type == POLYGON_);
	}
};

typedef boost::intrusive_ptr<OutputObject> OutputObjectRef;

static inline void intrusive_ptr_add_ref(OutputObject *oo){
	auto result = oo->references.fetch_add(1, std::memory_order_relaxed);
}

static inline void intrusive_ptr_release(OutputObject *oo) {
	if (oo->references.fetch_sub(1, std::memory_order_release) == 1) {
      std::atomic_thread_fence(std::memory_order_acquire);
      delete oo;
    }
}

/** \brief Assemble a linestring or polygon into a Boost geometry, and clip to bounding box
 * Returns a boost::variant -
 *	 POLYGON->MultiPolygon, CENTROID->Point, LINESTRING->MultiLinestring
 */
Geometry buildWayGeometry(OSMStore &osmStore, OutputObject const &oo, const TileBbox &bbox);

//\brief Build a node geometry
LatpLon buildNodeGeometry(OSMStore &osmStore, OutputObject const &oo, const TileBbox &bbox);

// Comparison functions

bool operator==(const OutputObjectRef &x, const OutputObjectRef &y);

/**
 * Do lexicographic comparison, with the order of: layer, geomType, attributes, and objectID.
 * Note that attributes is preferred to objectID.
 * It is to arrange objects with the identical attributes continuously.
 * Such objects will be merged into one object, to reduce the size of output.
 */
bool operator<(const OutputObjectRef &x, const OutputObjectRef &y);

namespace vector_tile {
	bool operator==(const vector_tile::Tile_Value &x, const vector_tile::Tile_Value &y);
	bool operator<(const vector_tile::Tile_Value &x, const vector_tile::Tile_Value &y);
}

namespace std {
	/// Hashing function so we can use an unordered_set
	template<>
	struct hash<OutputObjectRef> {
		size_t operator()(const OutputObjectRef &oo) const {
			return std::hash<uint_least8_t>()(oo->layer) ^ std::hash<NodeID>()(oo->objectID);
		}
	};
}

#endif //_OUTPUT_OBJECT_H
