/*
	OutputObject - any object (node, linestring, polygon) to be outputted to tiles
*/

#include "output_object.h"
#include "helpers.h"
#include "coordinates_geom.h"
#include <iostream>
using namespace std;
namespace geom = boost::geometry;

// **********************************************************

std::ostream& operator<<(std::ostream& os, OutputGeometryType geomType)
{
	switch(geomType) {
		case POINT_:
			os << "POINT";
			break;
		case LINESTRING_:
			os << "LINESTRING";
			break;
		case MULTILINESTRING_:
			os << "MULTILINESTRING";
			break;
		case POLYGON_:
			os << "POLYGON";
			break;
	}

	return os;
}

void OutputObject::writeAttributes(
	const AttributeStore& attributeStore,
	vtzero::feature_builder& fbuilder,
	char zoom
) const {
	auto attr = attributeStore.getUnsafe(attributes);

	for(auto const &it: attr) {
		if (it->minzoom > zoom) continue;

		// TODO: consider taking a data view that is stable
		// Look for key
		const std::string& key = attributeStore.keyStore.getKeyUnsafe(it->keyIndex);
		
		if (it->hasStringValue()) {
			fbuilder.add_property(key, it->stringValue());
		} else if (it->hasBoolValue()) {
			fbuilder.add_property(key, it->boolValue());
		} else if (it->hasIntValue()) {
			// could potentially add ,vtzero::sint_value_type(2) to force sint encoding (efficient for -ve ints)
			fbuilder.add_property(key, it->intValue());
		} else if (it->hasFloatValue()) {
			fbuilder.add_property(key, it->floatValue());
		}
	}
}

bool OutputObject::compatible(const OutputObject &other) {
	return geomType == other.geomType &&
	       z_order == other.z_order &&
	       attributes == other.attributes;
}

// Comparision functions
bool operator==(const OutputObject& x, const OutputObject& y) {
	return
		x.layer == y.layer &&
		x.z_order == y.z_order &&
		x.geomType == y.geomType &&
		x.attributes == y.attributes &&
		x.objectID == y.objectID;
}

bool operator==(const OutputObjectID& x, const OutputObjectID& y) {
	return x.oo == y.oo && x.id == y.id;
}
