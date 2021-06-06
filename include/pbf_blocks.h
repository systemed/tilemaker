/*! \file */ 
#ifndef _PBF_BLOCKS_H
#define _PBF_BLOCKS_H

#include <vector>
#include <string>
#include <map>
#include <fstream>

// Protobuf
#include "osmformat.pb.h"
#include "vector_tile.pb.h"

/* -------------------
   Protobuf handling
   ------------------- */

// Read and parse a protobuf message
void readMessage(google::protobuf::Message *message, std::istream &input, unsigned int size);

// Read an osm.pbf sequence of header length -> BlobHeader -> Blob
// and parse the unzipped contents into a message
BlobHeader readHeader(std::istream &input);
void readBlock(google::protobuf::Message *messagePtr, std::size_t datasize, std::istream &input);

void writeBlock(google::protobuf::Message *messagePtr, std::ostream &output, std::string headerType);
/* -------------------
   Tag handling
   ------------------- */

// Populate an array with the contents of a StringTable
void readStringTable(std::vector<std::string> *strPtr, PrimitiveBlock *pbPtr);

/// Populate a map with the reverse contents of a StringTable (i.e. string->num)
void readStringMap(std::map<std::string, int> *mapPtr, PrimitiveBlock *pbPtr);

/// Read the tags for a way into a hash
/// requires strings array to have been populated by readStringTable
std::map<std::string, std::string> getTags(std::vector<std::string> *strPtr, Way *wayPtr);

/// Find the index of a string in the StringTable, adding it if it's not there
unsigned int findStringInTable(std::string *strPtr, std::map<std::string, int> *mapPtr, PrimitiveBlock *pbPtr);

/// Set a tag for a way to a new value
void setTag(Way *wayPtr, unsigned int keyIndex, unsigned int valueIndex);

#endif //_PBF_BLOCKS_H

