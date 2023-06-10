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
	vector_tile::Tile_Feature *featurePtr,
	char zoom) const {

	for(auto const &it: attributes->values) {
		if (it.minzoom > zoom) continue;

		// Look for key
		std::string const &key = it.key;
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
		vector_tile::Tile_Value const &value = it.value;
		int subscript = findValue(valueList, value);
		if (subscript>-1) {
			featurePtr->add_tags(subscript);
		} else {
			uint32_t subscript = valueList->size();
			valueList->push_back(value);
			featurePtr->add_tags(subscript);
		}

		//if(value.has_string_value())
		//	std::cout << "Write attr: " << key << " " << value.string_value() << std::endl;	
	}
}

// Find a value in the value dictionary
// (we can't easily use find() because of the different value-type encoding - 
//	should be possible to improve this though)
int OutputObject::findValue(vector<vector_tile::Tile_Value> *valueList, vector_tile::Tile_Value const &value) const {
	for (size_t i=0; i<valueList->size(); i++) {
		vector_tile::Tile_Value v = valueList->at(i);
		if (v.has_string_value() && value.has_string_value() && v.string_value()==value.string_value()) { return i; }
		if (v.has_float_value()  && value.has_float_value()  && v.float_value() ==value.float_value() ) { return i; }
		if (v.has_bool_value()	 && value.has_bool_value()   && v.bool_value()  ==value.bool_value()	) { return i; }
	}
	return -1;
}

// Comparision functions

bool operator==(const OutputObjectRef x, const OutputObjectRef y) {
	return
		x->layer == y->layer &&
		x->z_order == y->z_order &&
		x->geomType == y->geomType &&
		x->attributes == y->attributes &&
		x->objectID == y->objectID;
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
