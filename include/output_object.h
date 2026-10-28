/*! \file */ 
#ifndef _OUTPUT_OBJECT_H
#define _OUTPUT_OBJECT_H

#include <vector>
#include <string>
#include <map>
#include <memory>
#include "coordinates.h"
#include "attribute_store.h"
#include <vtzero/builder.hpp>

enum OutputGeometryType : unsigned int { POINT_, LINESTRING_, MULTILINESTRING_, POLYGON_ };

//\brief Display the geometry type
std::ostream& operator<<(std::ostream& os, OutputGeometryType geomType);

/**
 * \brief OutputObject - any object (node, linestring, polygon) to be outputted to tiles
*/
#pragma pack(push, 4)
class OutputObject {

public:
	OutputObject(
		OutputGeometryType type,
		uint_least8_t l,
		NodeID id,
		AttributeIndex attributes,
		unsigned int mz
	):
		objectID(id),
		geomType(type),
		layer(l),
		z_order(0),
		minZoom(mz),
		attributes(attributes)
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

	// memo (MVT only, optional) must belong to the layer_builder that fbuilder writes to
	void writeAttributes(
		const AttributeStore& attributeStore,
		vtzero::feature_builder& fbuilder,
		char zoom,
		class MvtAttributeMemo* memo = nullptr
	) const;
#ifdef TILEMAKER_WITH_MLT
	void writeAttributes(
		const AttributeStore& attributeStore,
		class MltFeatureBuilder& fbuilder,
		char zoom,
		class MvtAttributeMemo* memo = nullptr // unused
	) const;
#endif

	bool compatible(const OutputObject &other);

};
#pragma pack(pop)

// Remembers, for one vtzero::layer_builder at one zoom, the key/value table
// indices that each attribute set's properties resolve to, so that features
// sharing an attribute set don't re-encode and re-find their keys and values.
// layer_builder::add_key/add_value are find-or-append, so a remembered index
// is exactly what vtzero would return again: the tile bytes are unchanged.
// Call reset() whenever a new layer_builder is started.
class MvtAttributeMemo {
public:
	void reset(vtzero::layer_builder& layer, char zoom);

private:
	friend class OutputObject;

	// Open-addressing uint32_t -> (a, b) map. clear() is O(1): slots carry a generation.
	struct Table {
		struct Slot { uint32_t gen, key, a, b; };
		std::vector<Slot> slots;
		uint32_t gen = 1, used = 0;

		const Slot* find(uint32_t key) const;
		void insert(uint32_t key, uint32_t a, uint32_t b); // key must not be present
		void clear();
	};

	// Returns the [key, value] index pairs for the set (count pairs)
	const uint32_t* resolve(const AttributeStore& attributeStore, AttributeIndex index, uint32_t& count);
	void resolvePair(const AttributeStore& attributeStore, uint32_t pairIndex, uint32_t& key, uint32_t& value);

	vtzero::layer_builder* layer = nullptr;
	char zoom = 0;
	uint32_t layerGen = 0;  // validity of keyGen entries
	Table sets;             // AttributeIndex -> [begin, end) in entries
	Table pairs;            // pair index -> key, value index (key NO_INDEX: not written at this zoom)
	std::vector<uint32_t> entries;
	std::vector<uint32_t> keyGen, keyIdx; // by AttributeKeyStore index
};

struct OutputObjectID {
	OutputObject oo;
	uint64_t id;
};

// Comparison functions
bool operator==(const OutputObject& x, const OutputObject& y);
bool operator==(const OutputObjectID& x, const OutputObjectID& y);

#endif //_OUTPUT_OBJECT_H
