/*
	OutputObject - any object (node, linestring, polygon) to be outputted to tiles
*/

#include "output_object.h"
#include "helpers.h"
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


// Write attribute key/value pairs (dictionary-encoded)
void OutputObject::writeAttributes(
	vector<string> *keyList, 
	vector<vector_tile::Tile_Value> *valueList, 
	AttributeStore const &attributeStore,
	vector_tile::Tile_Feature *featurePtr,
	char zoom) const {

	auto attr = attributeStore.getUnsafe(attributes);

	for(auto const &it: attr) {
		if (it->minzoom > zoom) continue;

		// Look for key
		std::string const &key = attributeStore.keyStore.getKeyUnsafe(it->keyIndex);
		auto kt = find(keyList->begin(), keyList->end(), key);
		if (kt != keyList->end()) {
			uint32_t subscript = kt - keyList->begin();
			featurePtr->add_tags(subscript);
		} else {
			uint32_t subscript = keyList->size();
			keyList->push_back(key);
			featurePtr->add_tags(subscript);
		}
		
		// Look for value
		int subscript = findValue(valueList, *it);
		if (subscript>-1) {
			featurePtr->add_tags(subscript);
		} else {
			uint32_t subscript = valueList->size();
			vector_tile::Tile_Value value;
			if (it->hasStringValue()) {
				value.set_string_value(it->stringValue());
			} else if (it->hasBoolValue()) {
				value.set_bool_value(it->boolValue());
			} else if (it->hasFloatValue()) {
				value.set_float_value(it->floatValue());
			}
			
			valueList->push_back(value);
			featurePtr->add_tags(subscript);
		}

		//if(value.hasStringValue())
		//	std::cout << "Write attr: " << key << " " << value.string_value() << std::endl;	
	}
}

// Find a value in the value dictionary
// (we can't easily use find() because of the different value-type encoding - 
//	should be possible to improve this though)
int OutputObject::findValue(const vector<vector_tile::Tile_Value>* valueList, const AttributePair& value) const {
	for (size_t i=0; i<valueList->size(); i++) {
		const vector_tile::Tile_Value& v = valueList->at(i);
		if (v.has_string_value() && value.hasStringValue() && v.string_value()==value.stringValue()) { return i; }
		if (v.has_float_value()  && value.hasFloatValue()  && v.float_value() ==value.floatValue() ) { return i; }
		if (v.has_bool_value()	 && value.hasBoolValue()   && v.bool_value()  ==value.boolValue()	) { return i; }
	}
	return -1;
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

namespace vector_tile {
	bool operator==(const vector_tile::Tile_Value &x, const vector_tile::Tile_Value &y) {
		std::string strx = x.SerializeAsString();
		std::string stry = y.SerializeAsString();
		return strx == stry;
	}
	bool operator<(const vector_tile::Tile_Value &x, const vector_tile::Tile_Value &y) {
		std::string strx = x.SerializeAsString();
		std::string stry = y.SerializeAsString();
		return strx < stry;
	}
}
