#include "attribute_store.h"

// AttributeSet

void AttributeSet::add(AttributePair const &kv) {
	lock();
	values.insert(kv);
	unlock();
}
void AttributeSet::add(std::string const &key, vector_tile::Tile_Value const &v, char minzoom) {
	AttributePair kv(key,v,minzoom);
	lock();
	values.insert(kv);
	unlock();
}

// AttributeStore

AttributeIndex AttributeStore::add(AttributeSet const &attributes) {
	std::lock_guard<std::mutex> lock(mutex);

	// Do we already have it?
	auto existing = attribute_indices.find(attributes);
	if (existing != attribute_indices.end()) return existing->second;

	// No, so add and return the index
	AttributeIndex idx = static_cast<AttributeIndex>(attribute_sets.size());
	attribute_sets.emplace_back(attributes);
	attribute_indices.emplace(attributes, idx);
	return idx;
}

std::set<AttributePair, AttributeSet::key_value_less> AttributeStore::get(AttributeIndex index) const {
	try {
		return attribute_sets[index].values;
	} catch (std::out_of_range &err) {
		throw std::runtime_error("Failed to fetch attributes at index "+std::to_string(index)+" - size is "+std::to_string(attribute_sets.size()));
	}
}

void AttributeStore::reportSize() const {
	std::cout << "Attributes: " << attribute_sets.size() << " sets" << std::endl;
}

void AttributeStore::doneReading() {
	// once we've finished reading, we don't need the unordered_map any more
	attribute_indices.clear();
}
