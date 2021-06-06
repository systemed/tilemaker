#include "pbf_blocks.h"
#include "helpers.h"
#include <fstream>
using namespace std;

/* -------------------
   Protobuf handling
   ------------------- */

// Read and parse a protobuf message
void readMessage(google::protobuf::Message *message, istream &input, unsigned int size) {
	vector<char> buffer(size);
	input.read(&buffer.front(), size);
	message->ParseFromArray(&buffer.front(), size);
}

// Read an osm.pbf sequence of header length -> BlobHeader -> Blob
// and parse the unzipped contents into a message
BlobHeader readHeader(istream &input) {
	BlobHeader bh;

	unsigned int size;
	input.read((char*)&size, sizeof(size));
	if (input.eof()) { return bh; }
	endian_swap(size);

	// get BlobHeader and parse
	readMessage(&bh, input, size);
	return bh;
}

void readBlock(google::protobuf::Message *messagePtr, std::size_t datasize, istream &input) {
	if (input.eof()) { return ; }

	// get Blob and parse
	Blob blob;
	readMessage(&blob, input, datasize);

	// Unzip the gzipped content
	string contents = decompress_string(blob.zlib_data(), false);
	messagePtr->ParseFromString(contents);
}

void writeBlock(google::protobuf::Message *messagePtr, ostream &output, string headerType) {
	// encode the message
	string serialised;
	messagePtr->SerializeToString(&serialised);
	// create a blob and store it
	Blob blob;
	blob.set_raw_size(serialised.length());
	blob.set_zlib_data(compress_string(serialised));
	// encode the blob
	string blob_encoded;
	blob.SerializeToString(&blob_encoded);
	
	// create the BlobHeader
	BlobHeader bh;
	bh.set_type(headerType);
	bh.set_datasize(blob_encoded.length());
	// encode it
	string header_encoded;
	bh.SerializeToString(&header_encoded);
	
	// write out
	unsigned int bhLength=header_encoded.length();
	endian_swap(bhLength);
	output.write(reinterpret_cast<const char *>(&bhLength), 4);
	output.write(header_encoded.c_str(), header_encoded.length() );
	output.write(blob_encoded.c_str(), blob_encoded.length() );
}

/* -------------------
   Tag handling
   ------------------- */

// Populate an array with the contents of a StringTable
void readStringTable(vector<string> *strPtr, PrimitiveBlock *pbPtr) {
	strPtr->resize(pbPtr->stringtable().s_size());
	for (int i=0; i<pbPtr->stringtable().s_size(); i++) {
		(*strPtr)[i] = pbPtr->stringtable().s(i);			// dereference strPtr to get strings
	}
}

// Populate a map with the reverse contents of a StringTable (i.e. string->num)
void readStringMap(map<string, int> *mapPtr, PrimitiveBlock *pbPtr) {
	for (int i=0; i<pbPtr->stringtable().s_size(); i++) {
		mapPtr->insert(pair<string, int> (pbPtr->stringtable().s(i), i));
	}
}

// Read the tags for a way into a hash
// requires strings array to have been populated by readStringTable
map<string, string> getTags(vector<string> *strPtr, Way *wayPtr) {
	map<string, string> tags;
	for (int n=0; n<wayPtr->keys_size(); n++) {
		tags[(*strPtr)[wayPtr->keys(n)]] = (*strPtr)[wayPtr->vals(n)];
	}
	return tags;
}

// Find the index of a string in the StringTable, adding it if it's not there
unsigned int findStringInTable(string *strPtr, map<string, int> *mapPtr, PrimitiveBlock *pbPtr) {
	if (mapPtr->find(*strPtr) == mapPtr->end()) {
		pbPtr->mutable_stringtable()->add_s(*strPtr);
		unsigned int ix = pbPtr->stringtable().s_size()-1;
		mapPtr->insert(pair<string, int> (*strPtr, ix));
	}
	return mapPtr->at(*strPtr);
}

// Set a tag for a way to a new value
void setTag(Way *wayPtr, unsigned int keyIndex, unsigned int valueIndex) {
	for (int i=0; i<wayPtr->keys_size(); i++) {
		if (wayPtr->keys(i)==keyIndex) {
			wayPtr->mutable_vals()->Set(i,valueIndex);
			return;
		}
	}
	wayPtr->mutable_keys()->Add(keyIndex);
	wayPtr->mutable_vals()->Add(valueIndex);
}
