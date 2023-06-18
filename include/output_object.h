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

#ifdef FLOAT_Z_ORDER
typedef float ZOrder;
#else
typedef int8_t ZOrder;
#endif

enum OutputGeometryType : unsigned int { POINT_, LINESTRING_, MULTILINESTRING_, POLYGON_ };

#define OSMID_TYPE_OFFSET	40
#define OSMID_MASK 		((1ULL<<OSMID_TYPE_OFFSET)-1)
#define OSMID_SHAPE 	(0ULL<<OSMID_TYPE_OFFSET)
#define OSMID_NODE 		(1ULL<<OSMID_TYPE_OFFSET)
#define OSMID_WAY 		(2ULL<<OSMID_TYPE_OFFSET)
#define OSMID_RELATION 	(3ULL<<OSMID_TYPE_OFFSET)

//\brief Display the geometry type
std::ostream& operator<<(std::ostream& os, OutputGeometryType geomType);

/**
 * \brief OutputObject - any object (node, linestring, polygon) to be outputted to tiles

 * Possible future improvements to save memory:
 * - use a global dictionary for attribute key/values
*/
#pragma pack(push, 4)
class OutputObject {

protected:	
	OutputObject(OutputGeometryType type, uint_least8_t l, NodeID id, AttributeIndex attributes, uint mz) 
		: objectID(id), geomType(type), layer(l), z_order(0),
		  minZoom(mz), attributes(attributes)
	{ }


public:
	NodeID objectID 			: 42;					// id of way (linestring/polygon) or node (point)
	OutputGeometryType geomType : 2;					// point, linestring, polygon
	unsigned minZoom 			: 4;					// minimum zoom level in which object is written
	uint_least8_t layer 		: 8;					// what layer is it in?
	ZOrder z_order				;						// used for sorting features within layers

	AttributeIndex attributes;

	void setZOrder(const ZOrder z) {
#ifndef FLOAT_Z_ORDER
		if (z <= -127 || z >= 127) {
			throw std::runtime_error("z_order is limited to 1 byte signed integer.");
		}
#endif
		z_order = z;
	}

	void setMinZoom(unsigned z) {
		minZoom = z;
	}

	void setAttributeSet(AttributeIndex attributes) {
		this->attributes = attributes;
	}

	//\brief Write attribute key/value pairs (dictionary-encoded)
	void writeAttributes(std::vector<std::string> *keyList, 
		std::vector<vector_tile::Tile_Value> *valueList, 
		AttributeStore const &attributeStore,
		vector_tile::Tile_Feature *featurePtr, char zoom) const;
	
	/**
	 * \brief Find a value in the value dictionary
	 * (we can't easily use find() because of the different value-type encoding - 
	 *	should be possible to improve this though)
	 */
	int findValue(std::vector<vector_tile::Tile_Value> *valueList, vector_tile::Tile_Value const &value) const;
};
#pragma pack(pop)

/**
 * \brief An OutputObject derived class that contains data originally from OsmMemTiles
*/
class OutputObjectPoint : public OutputObject
{
public:
	OutputObjectPoint(OutputGeometryType type, uint_least8_t l, NodeID id, AttributeIndex attributes, uint minzoom)
		: OutputObject(type, l, id, attributes, minzoom)
	{ 
		assert(type == POINT_);
	}
}; 

class OutputObjectLinestring : public OutputObject
{
public:
	OutputObjectLinestring(OutputGeometryType type, uint_least8_t l, NodeID id, AttributeIndex attributes, uint minzoom)
		: OutputObject(type, l, id, attributes, minzoom)
	{ 
		assert(type == LINESTRING_);
	}
};

class OutputObjectMultiLinestring : public OutputObject
{
public:
	OutputObjectMultiLinestring(OutputGeometryType type, uint_least8_t l, NodeID id, AttributeIndex attributes, uint minzoom)
		: OutputObject(type, l, id, attributes, minzoom)
	{ 
		assert(type == MULTILINESTRING_);
	}
};


class OutputObjectMultiPolygon : public OutputObject
{
public:
	OutputObjectMultiPolygon(OutputGeometryType type, uint_least8_t l, NodeID id, AttributeIndex attributes, uint minzoom)
		: OutputObject(type, l, id, attributes, minzoom)
	{ 
		assert(type == POLYGON_);
	}
};

class OutputObjectRef
{
	OutputObject *oo;

public:
	OutputObjectRef(OutputObject *oo = nullptr)
		: oo(oo)
	{ }
	OutputObjectRef(OutputObjectRef const &other) = default;
	OutputObjectRef(OutputObjectRef &&other) = default;

	OutputObjectRef &operator=(OutputObjectRef const &other) { oo = other.oo; return *this; }
    OutputObject& operator*() { return *oo; }
    OutputObject const& operator*() const { return *oo; }
    OutputObject *operator->() { return oo; }
    OutputObject const *operator->() const { return oo; }
	void reset() { oo = nullptr; }
};

typedef std::deque<std::pair<OutputObjectRef, AttributeSet>> OutputRefsWithAttributes;

// Comparison functions

bool operator==(const OutputObjectRef x, const OutputObjectRef y);

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
