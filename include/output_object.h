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
		uint mz
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

	void writeAttributes(
		const AttributeStore& attributeStore,
		vtzero::feature_builder& fbuilder,
		char zoom
	) const;
		
	bool compatible(const OutputObject &other);

};
#pragma pack(pop)

struct OutputObjectID {
	OutputObject oo;
	uint64_t id;
};

// Comparison functions
bool operator==(const OutputObject& x, const OutputObject& y);
bool operator==(const OutputObjectID& x, const OutputObjectID& y);

#endif //_OUTPUT_OBJECT_H
