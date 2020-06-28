#include "attribute_store.h"

#include <iterator>

// Get the index for a key/value pair, inserting it into the dictionary if it's not already present
unsigned AttributeStore::indexForPair(const std::string &k, const vector_tile::Tile_Value &v, bool isShapefile) {

	AttributePair ap(0,k,v);
 	std::unordered_set<AttributePair> &attributes = isShapefile ? shpAttributes : osmAttributes;

	auto it = attributes.find(ap);
	if (it==attributes.end()) {
		ap.index = isShapefile ? shpAttributeCount : osmAttributeCount;
		attributes.insert(ap);
		isShapefile ? shpAttributeCount++ : osmAttributeCount++;
		return ap.index;
	} else {
		return it->index;
	}
}

// Sort into vectors
void AttributeStore::sortOsmAttributes() {
	sortedOsmAttributes.insert(
		sortedOsmAttributes.end(),
		std::make_move_iterator(osmAttributes.begin()),
		std::make_move_iterator(osmAttributes.end())
	);
	osmAttributes = std::unordered_set<AttributePair>(); // clear
	std::sort(sortedOsmAttributes.begin(), sortedOsmAttributes.end(),
		[](const AttributePair &a, const AttributePair &b) { return a.index < b.index; }
	);
}
void AttributeStore::sortShpAttributes() {
	sortedShpAttributes.insert(
		sortedShpAttributes.end(),
		std::make_move_iterator(shpAttributes.begin()),
		std::make_move_iterator(shpAttributes.end())
	);
	shpAttributes = std::unordered_set<AttributePair>(); // clear
	std::sort(sortedShpAttributes.begin(), sortedShpAttributes.end(),
		[](const AttributePair &a, const AttributePair &b) { return a.index < b.index; }
	);
}

// Get k/v pair at a given index
AttributePair AttributeStore::pairAtIndex(unsigned i, bool isShapefile) const {
	return isShapefile ? sortedShpAttributes[i] : sortedOsmAttributes[i];
}

// Clear OSM attributes
void AttributeStore::clearOsmAttributes() {
	osmAttributes.clear();
	sortedOsmAttributes.clear();
	osmAttributeCount = 0;
}
