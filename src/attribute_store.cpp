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
	lookups++;

	// Do we already have it?
	auto existing = attribute_sets.find(attributes);
	if (existing != attribute_sets.end()) return existing - attribute_sets.begin();

	// No, so add and return the index
	AttributeIndex idx = static_cast<AttributeIndex>(attribute_sets.size());
	attribute_sets.insert(attributes);
	return idx;
}

std::set<AttributePair, AttributeSet::key_value_less> AttributeStore::get(AttributeIndex index) const {
	try {
		return attribute_sets.nth(index).key().values;
	} catch (std::out_of_range &err) {
		throw std::runtime_error("Failed to fetch attributes at index "+std::to_string(index)+" - size is "+std::to_string(attribute_sets.size()));
	}
}

void AttributeStore::reportSize() const {
	std::cout << "Attributes: " << attribute_sets.size() << " sets from " << lookups << " objects" << std::endl;
}

void AttributeStore::doneReading() {
}
