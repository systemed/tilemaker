/*! \file */ 
#ifndef _ATTRIBUTE_STORE_H
#define _ATTRIBUTE_STORE_H

#include "vector_tile.pb.h"
#include <unordered_set>

/*	AttributePair
	a key/value combination for an OutputObject */

class AttributePair {

public:
	
	unsigned index;
	std::string key;
	vector_tile::Tile_Value value;

	// constructor
	AttributePair(unsigned i, std::string k, vector_tile::Tile_Value v) {
		index=i; key=k; value=v;
	}

	// comparison
	bool operator==(const AttributePair &other) const { 
	    if (key==other.key) {
			if (value.has_string_value()) {
				return (other.value.has_string_value() && value.string_value()==other.value.string_value());
			} else if (value.has_float_value()) {
				return (other.value.has_float_value() && value.float_value()==other.value.float_value());
			} else if (value.has_bool_value()) {
				return (other.value.has_bool_value() && value.bool_value()==other.value.bool_value());
			}
		}
		return false;
	}
};

// hash function for AttributePair
namespace std {
	template<> struct hash<AttributePair> {
		size_t operator()(const AttributePair &obj) const {
			size_t h;
			if      (obj.value.has_string_value()) h = hash<std::string>()(obj.value.string_value());
			else if (obj.value.has_float_value())  h = hash<float>()(obj.value.float_value());
			else if (obj.value.has_bool_value())   h = hash<bool>()(obj.value.bool_value());
			return hash<std::string>()(obj.key) ^ h;
		}
	};
}


/*	AttributeStore

	global dictionaries for attribute pairs
	design is basically as per https://github.com/systemed/tilemaker/issues/163#issuecomment-633062333
	note that we have two dictionaries (one for OSM objects, one for shapefiles) 
	  because we need to clear/reread the OSM objects when used with mapsplit
*/

class AttributeStore {

public:

	std::unordered_set<AttributePair> osmAttributes;
	std::unordered_set<AttributePair> shpAttributes;
	unsigned osmAttributeCount = 0;
	unsigned shpAttributeCount = 0;

	std::vector<AttributePair> sortedOsmAttributes;
	std::vector<AttributePair> sortedShpAttributes;

	unsigned indexForPair(const std::string &k, const vector_tile::Tile_Value &v, bool isShapefile);
	void sortOsmAttributes();
	void sortShpAttributes();
	AttributePair pairAtIndex(unsigned i, bool isShapefile) const;
	void clearOsmAttributes();
	
};

#endif //_COORDINATES_H
