/*
	OutputObject - any object (node, linestring, polygon) to be outputted to tiles
*/

#include "output_object.h"
#include "helpers.h"
#include "coordinates_geom.h"
#include <iostream>
#include <algorithm>
#ifdef TILEMAKER_WITH_MLT
#include "mlt_writer.h"
#endif
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

template<class FeatureBuilder>
static void writeAttributesTo(
	const AttributeStore& attributeStore,
	AttributeIndex attributes,
	FeatureBuilder& fbuilder,
	char zoom
) {
	attributeStore.forEachPairUnsafe(attributes, [&](const AttributePair& pair) {
		const AttributePair* it = &pair;
		if (it->minzoom > zoom) return;

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
	});
}

// MvtAttributeMemo

static const uint32_t MEMO_MAX_ENTRIES = 1 << 16; // key/value indices, i.e. 32K properties
static const uint32_t MEMO_MAX_SETS = 1 << 13;
static const uint32_t MEMO_MAX_PAIRS = 1 << 14;
static const size_t MEMO_KEYS = 1 << 9; // AttributePair::keyIndex is 9 bits
static const uint32_t NO_INDEX = ~0u;

static inline uint32_t memoHash(uint32_t x) {
	// AttributeIndex/pair index is shard << 18 | offset: mix the high bits into the low ones
	x ^= x >> 16; x *= 0x7feb352dU;
	x ^= x >> 15; x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

const MvtAttributeMemo::Table::Slot* MvtAttributeMemo::Table::find(uint32_t key) const {
	if (slots.empty()) return nullptr;
	const size_t mask = slots.size() - 1;
	size_t i = memoHash(key) & mask;
	while (slots[i].gen == gen) {
		if (slots[i].key == key) return &slots[i];
		i = (i + 1) & mask;
	}
	return nullptr;
}

void MvtAttributeMemo::Table::insert(uint32_t key, uint32_t a, uint32_t b) {
	if ((used + 1) * 2 > slots.size()) {
		std::vector<Slot> old;
		old.swap(slots);
		slots.assign(old.empty() ? 256 : old.size() * 2, Slot{0, 0, 0, 0});
		const size_t mask = slots.size() - 1;
		for (const Slot& s : old) {
			if (s.gen != gen) continue;
			size_t i = memoHash(s.key) & mask;
			while (slots[i].gen == gen) i = (i + 1) & mask;
			slots[i] = s;
		}
	}
	const size_t mask = slots.size() - 1;
	size_t i = memoHash(key) & mask;
	while (slots[i].gen == gen) i = (i + 1) & mask;
	slots[i] = Slot{gen, key, a, b};
	used++;
}

void MvtAttributeMemo::Table::clear() {
	used = 0;
	if (++gen == 0) {
		for (Slot& s : slots) s.gen = 0;
		gen = 1;
	}
}

void MvtAttributeMemo::reset(vtzero::layer_builder& layer, char zoom) {
	this->layer = &layer;
	this->zoom = zoom;
	if (keyGen.empty()) {
		keyGen.assign(MEMO_KEYS, 0);
		keyIdx.assign(MEMO_KEYS, 0);
	}
	if (++layerGen == 0) {
		std::fill(keyGen.begin(), keyGen.end(), 0);
		layerGen = 1;
	}
	sets.clear();
	pairs.clear();
	entries.clear();
}

// Adds the pair's key and value to the layer tables exactly as writeAttributesTo would
void MvtAttributeMemo::resolvePair(const AttributeStore& attributeStore, uint32_t pairIndex, uint32_t& key, uint32_t& value) {
	key = NO_INDEX;
	value = NO_INDEX;
	const AttributePair& it = attributeStore.pairStore.getPairUnsafe(pairIndex);
	if (it.minzoom > zoom) return;

	if (it.hasStringValue()) {
		const PooledString& ps = it.pooledString();
		value = layer->add_value(vtzero::encoded_property_value{vtzero::data_view{ps.data(), ps.size()}}).value();
	} else if (it.hasBoolValue()) {
		value = layer->add_value(vtzero::encoded_property_value{it.boolValue()}).value();
	} else if (it.hasIntValue()) {
		value = layer->add_value(vtzero::encoded_property_value{it.intValue()}).value();
	} else if (it.hasFloatValue()) {
		value = layer->add_value(vtzero::encoded_property_value{it.floatValue()}).value();
	} else {
		return;
	}

	const uint16_t keyIndex = it.keyIndex;
	if (keyGen[keyIndex] == layerGen) {
		key = keyIdx[keyIndex];
	} else {
		const std::string& keyString = attributeStore.keyStore.getKeyUnsafe(keyIndex);
		key = layer->add_key(vtzero::data_view{keyString.data(), keyString.size()}).value();
		keyGen[keyIndex] = layerGen;
		keyIdx[keyIndex] = key;
	}
}

const uint32_t* MvtAttributeMemo::resolve(const AttributeStore& attributeStore, AttributeIndex index, uint32_t& count) {
	if (const Table::Slot* s = sets.find(index)) {
		count = (s->b - s->a) / 2;
		return entries.data() + s->a;
	}

	// Miss: resolve each pair in order (the first time it is seen in this layer)
	if (entries.size() >= MEMO_MAX_ENTRIES || sets.used >= MEMO_MAX_SETS) {
		entries.clear();
		sets.clear();
	}
	if (pairs.used >= MEMO_MAX_PAIRS)
		pairs.clear();
	const uint32_t begin = entries.size();
	attributeStore.forEachPairIndexUnsafe(index, [&](uint32_t pairIndex) {
		uint32_t key, value;
		if (const Table::Slot* s = pairs.find(pairIndex)) {
			key = s->a;
			value = s->b;
		} else {
			resolvePair(attributeStore, pairIndex, key, value);
			pairs.insert(pairIndex, key, value);
		}
		if (key == NO_INDEX) return;
		entries.push_back(key);
		entries.push_back(value);
	});
	const uint32_t end = entries.size();
	sets.insert(index, begin, end);

	count = (end - begin) / 2;
	return entries.data() + begin;
}

void OutputObject::writeAttributes(
	const AttributeStore& attributeStore,
	vtzero::feature_builder& fbuilder,
	char zoom,
	MvtAttributeMemo* memo
) const {
	if (!memo || !memo->layer || memo->zoom != zoom) {
		writeAttributesTo(attributeStore, attributes, fbuilder, zoom);
		return;
	}

	uint32_t count;
	const uint32_t* kv = memo->resolve(attributeStore, attributes, count);
	for (uint32_t i = 0; i < count; i++)
		fbuilder.add_property(vtzero::index_value_pair{kv[2*i], kv[2*i+1]});

}

#ifdef TILEMAKER_WITH_MLT
void OutputObject::writeAttributes(
	const AttributeStore& attributeStore,
	MltFeatureBuilder& fbuilder,
	char zoom,
	MvtAttributeMemo* /*memo*/
) const {
	writeAttributesTo(attributeStore, attributes, fbuilder, zoom);
}
#endif

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
