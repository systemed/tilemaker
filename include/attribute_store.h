/*! \file */ 
#ifndef _ATTRIBUTE_STORE_H
#define _ATTRIBUTE_STORE_H

#include "vector_tile.pb.h"
#include <unordered_set>
#include <boost/container/small_vector.hpp>
#include <algorithm>

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
    
    using key_value_store_t = std::set<kv_with_minzoom, key_value_less>;
    using key_value_store_iter_t = key_value_store_t::const_iterator;
    key_value_store_t key_values;
        
    struct key_value_store_less {
        bool operator()(key_value_store_iter_t lhs, key_value_store_iter_t rhs) const {            
			return (lhs->minzoom != rhs->minzoom) ? (lhs->minzoom < rhs->minzoom)
			     : (lhs->key != rhs->key) ? (lhs->key < rhs->key)
			     : compare(lhs->value, rhs->value);
        }
    }; 
    
	using key_value_set_entry_t = boost::container::small_vector<key_value_store_iter_t, 1>;
	using key_value_set_id_t = uint32_t;

	struct key_value_set_store_t {
		key_value_set_id_t id;
		key_value_set_entry_t entries; 
	};

    struct key_value_set_store_less {
        bool operator()(key_value_set_store_t const &lhs, key_value_set_store_t const &rhs) const {  
            if(lhs.entries.size() < rhs.entries.size()) return true;
            if(lhs.entries.size() > rhs.entries.size()) return false;
            
            key_value_store_less compare;
            for(auto i = lhs.entries.begin(), j = rhs.entries.begin(); i != lhs.entries.end(); ++i, ++j) {
                if(compare(*i, *j)) return true;
                if(compare(*j, *i)) return false;
            }
            
            return false;            
        }
    };     
    
    using key_value_set_t = std::set<key_value_set_store_t, key_value_set_store_less>;
	using key_value_set_iter_t = key_value_set_t::const_iterator;
    key_value_set_t set_list;

 
    AttributeStore()
		: empty_set_iter(store_set(key_value_set_entry_t()))
		, next_set_id(0) 
    { }
            
    key_value_store_iter_t store_key_value(std::string const &key, vector_tile::Tile_Value const &value, char const minZoom) {
        return key_values.insert({ key, value, minZoom }).first;              
    }
    
    key_value_set_iter_t store_set(key_value_set_entry_t set) {
        std::sort(set.begin(), set.end(), key_value_store_less());
        return set_list.insert( {next_set_id++, set} ).first;        
    }
    
	key_value_set_iter_t empty_set() const { return empty_set_iter; }

	/*
    void print() {
        std::cout << "Total sets: " << set_list.size() << std::endl;
        std::cout << "Total keys: " << keys.size() << std::endl;
        std::cout << "Total values: " << values.size() << std::endl;
        std::cout << "Total key values: " << key_values.size() << std::endl;

        for(auto const &i: set_list) {
            std::cout << "Set" << std::endl;
            for(auto const &j: i) {
                std::cout << *(j->first) << " ";
				
				if(j->second->has_bool_value())
					std::cout << std::boolalpha << j->second->bool_value() << std::endl;
				else if(j->second->has_float_value())
					std::cout << j->second->float_value() << std::endl;
				else if(j->second->has_string_value())
					std::cout << j->second->string_value() << std::endl;
            }
        } 
    }  */

private:
	key_value_set_iter_t empty_set_iter;
	key_value_set_id_t next_set_id;
};

using AttributeStoreRef = AttributeStore::key_value_set_iter_t;

#endif //_COORDINATES_H
