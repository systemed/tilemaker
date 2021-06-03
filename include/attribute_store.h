/*! \file */ 
#ifndef _ATTRIBUTE_STORE_H
#define _ATTRIBUTE_STORE_H

#include "vector_tile.pb.h"
#include <boost/functional/hash.hpp>

/*	AttributeStore 
 *	global dictionaries for attributes
 *	We combine all similair keys/values pairs and sets
 *	- All the same key/value pairs are combined in key_values
 *	- All the same set of key/values are combined in set_list
 *
 *	Every key/value set gets an ID in set_list. If the ID of two attribute sets are the same
 *	this means these objects share the same set of attribute/values. Output objects store a 
 *	reference to the set of paremeters in set_list. 
*/

struct AttributeStore
{
	struct kv_with_minzoom {
		std::string key;
		vector_tile::Tile_Value value;
		char minzoom;

		kv_with_minzoom(std::string const &key, vector_tile::Tile_Value const &value, char minzoom)
			: key(key), value(value), minzoom(minzoom)
		{ }  
	};

	enum class Index { BOOL, FLOAT, STRING };

	static Index type_index(vector_tile::Tile_Value const &v)
	{
		if(v.has_string_value())
			return Index::STRING;
		else if(v.has_float_value())
			return Index::FLOAT;
		else
			return Index::BOOL;
	}

	static bool compare(vector_tile::Tile_Value const &lhs, vector_tile::Tile_Value const &rhs) {
		auto lhs_id = type_index(lhs);
		auto rhs_id = type_index(lhs);

		if(lhs_id < rhs_id)
			return true;
		if(lhs_id > rhs_id)
			return false;

		switch(lhs_id) {
			case Index::BOOL:
				return lhs.bool_value() < rhs.bool_value();
			case Index::FLOAT:	
				return lhs.float_value() < rhs.float_value();
			case Index::STRING:	
				return lhs.string_value() < rhs.string_value();
		}

		throw std::runtime_error("Invalid type in attribute store");
	}
    
    struct key_value_less {
        bool operator()(kv_with_minzoom const &lhs, kv_with_minzoom const& rhs) const {            
			return (lhs.minzoom != rhs.minzoom) ? (lhs.minzoom < rhs.minzoom)
			     : (lhs.key != rhs.key) ? (lhs.key < rhs.key)
			     : compare(lhs.value, rhs.value);
        }
    }; 

	using key_value_set = std::set<kv_with_minzoom, key_value_less>;
	using key_value_set_ref_t = std::shared_ptr<key_value_set>;

    struct key_value_set_store_less {
        bool operator()(key_value_set_ref_t const &lhs, key_value_set_ref_t const &rhs) const {  
            if(lhs->size() < rhs->size()) return true;
            if(lhs->size() > rhs->size()) return false;
            
            key_value_less compare;
            for(auto i = lhs->begin(), j = rhs->begin(); i != lhs->end(); ++i, ++j) {
                if(compare(*i, *j)) return true;
                if(compare(*j, *i)) return false;
            }
            
            return false;            
        }
    };     

    using key_value_set_t = std::set<key_value_set_ref_t, key_value_set_store_less>;

	using key_value_index_t = std::pair<Index, char>; 
	using key_value_map_t = std::vector< std::pair<std::mutex, key_value_set_t> >;

	key_value_map_t set_list;

	AttributeStore(unsigned int threadNum) 
		: set_list(threadNum * threadNum)
	{ }

	key_value_set_ref_t empty_set() const { return std::make_shared<key_value_set>(); }

    key_value_set_ref_t store_set(key_value_set_ref_t attributes) {
		auto idx = attributes->size();
		for(auto i: *attributes) {
			boost::hash_combine(idx, i.minzoom);
			boost::hash_combine(idx, i.key);
			boost::hash_combine(idx, type_index(i.value));

			if(i.value.has_string_value())
				boost::hash_combine(idx, i.value.string_value());
			else if(i.value.has_float_value())
				boost::hash_combine(idx, i.value.float_value());
			else
				boost::hash_combine(idx, i.value.bool_value());
		} 
		
		std::lock_guard<std::mutex> lock(set_list[idx % set_list.size()].first);
		return *set_list[idx % set_list.size()].second.insert(attributes).first;	
	}
};

using AttributeStoreRef = AttributeStore::key_value_set_ref_t;

#endif //_COORDINATES_H
