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

enum OutputGeometryType : unsigned int { POINT_, LINESTRING_, MULTILINESTRING_, POLYGON_ };

//\brief Display the geometry type
std::ostream& operator<<(std::ostream& os, OutputGeometryType geomType);

/**
 * \brief OutputObject - any object (node, linestring, polygon) to be outputted to tiles

 * Possible future improvements to save memory:
 * - use a global dictionary for attribute key/values
*/
#pragma pack(push, 4)
class OutputObject {

public:
	OutputObject(OutputGeometryType type, uint_least8_t l, NodeID id, AttributeIndex attributes, uint mz) 
		: objectID(id), geomType(type), layer(l), z_order(0),
		  minZoom(mz), attributes(attributes)
	{ }


	NodeID objectID 			: 36;					// id of point/linestring/polygon
	unsigned minZoom 			: 4;					// minimum zoom level in which object is written
	AttributeIndex attributes   : 30;					// index in attribute storage
	OutputGeometryType geomType : 2;					// point, linestring, polygon
	uint_least8_t layer 		: 8;					// what layer is it in?
	short z_order				: 16;					// used for sorting features within layers

	template<typename T>
	static inline T finite_cast(double v) {
		if(!std::isfinite(v)) return 0;
		return static_cast<T>(std::floor(v));
	}

	void setZOrder(const double z) {
		if (z>1000) {
			z_order = finite_cast<short>(std::sqrt((z-1000)*10)+10000);
		} else if (z<-1000) {
			z_order = finite_cast<short>(-10000 - std::sqrt((std::fabs(z)-1000)*10));
		} else {
			z_order = finite_cast<short>(z*10);
		}
	}

	void setMinZoom(const double z) {
		minZoom = finite_cast<unsigned int>(z);
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
	int findValue(const std::vector<vector_tile::Tile_Value>* valueList, const AttributePair& value) const;
};
#pragma pack(pop)

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

typedef std::deque<std::pair<OutputObject, AttributeSet>> OutputObjectsWithAttributes;

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
