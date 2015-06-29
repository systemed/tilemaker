/* -------------------
   Protobuf handling
   ------------------- */

// Read and parse a protobuf message
void readMessage(google::protobuf::Message *message, fstream *input, unsigned int size) {
	vector<char> buffer(size);
	input->read(&buffer.front(), size);
	message->ParseFromArray(&buffer.front(), size);
}

// Read an osm.pbf sequence of header length -> BlobHeader -> Blob
// and parse the unzipped contents into a message
void readBlock(google::protobuf::Message *messagePtr, fstream *inputPtr) {
	// read the header length
	unsigned int size;
	inputPtr->read((char*)&size, sizeof(size));
	if (inputPtr->eof()) { return; }
	endian_swap(size);

	// get BlobHeader and parse
	BlobHeader bh;
	readMessage(&bh, inputPtr, size);

	// get Blob and parse
	Blob blob;
	readMessage(&blob, inputPtr, bh.datasize());

	// Unzip the gzipped content
	string contents = decompress_string(blob.zlib_data());
	messagePtr->ParseFromString(contents);
}

void writeBlock(google::protobuf::Message *messagePtr, fstream *outputPtr, string headerType) {
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
	uint bhLength=header_encoded.length();
	endian_swap(bhLength);
	outputPtr->write(reinterpret_cast<const char *>(&bhLength), 4);
	outputPtr->write(header_encoded.c_str(), header_encoded.length() );
	outputPtr->write(blob_encoded.c_str(), blob_encoded.length() );
}

/* -------------------
   Tag handling
   ------------------- */

// Populate an array with the contents of a StringTable
void readStringTable(vector<string> *strPtr, PrimitiveBlock *pbPtr) {
	strPtr->resize(pbPtr->stringtable().s_size());
	for (uint i=0; i<pbPtr->stringtable().s_size(); i++) {
		(*strPtr)[i] = pbPtr->stringtable().s(i);			// dereference strPtr to get strings
	}
}

// Populate a map with the reverse contents of a StringTable (i.e. string->num)
void readStringMap(map<string, int> *mapPtr, PrimitiveBlock *pbPtr) {
	for (uint i=0; i<pbPtr->stringtable().s_size(); i++) {
		mapPtr->insert(pair<string, int> (pbPtr->stringtable().s(i), i));
	}
}

// Read the tags for a way into a hash
// requires strings array to have been populated by readStringTable
map<string, string> getTags(vector<string> *strPtr, Way *wayPtr) {
	map<string, string> tags;
	for (uint n=0; n<wayPtr->keys_size(); n++) {
		tags[(*strPtr)[wayPtr->keys(n)]] = (*strPtr)[wayPtr->vals(n)];
	}
	return tags;
}

// Find the index of a string in the StringTable, adding it if it's not there
uint findStringInTable(string *strPtr, map<string, int> *mapPtr, PrimitiveBlock *pbPtr) {
	if (mapPtr->find(*strPtr) == mapPtr->end()) {
		pbPtr->mutable_stringtable()->add_s(*strPtr);
		uint ix = pbPtr->stringtable().s_size()-1;
		mapPtr->insert(pair<string, int> (*strPtr, ix));
	}
	return mapPtr->at(*strPtr);
}

// Set a tag for a way to a new value
void setTag(Way *wayPtr, uint keyIndex, uint valueIndex) {
	for (uint i=0; i<wayPtr->keys_size(); i++) {
		if (wayPtr->keys(i)==keyIndex) {
			wayPtr->mutable_vals()->Set(i,valueIndex);
			return;
		}
	}
	wayPtr->mutable_keys()->Add(keyIndex);
	wayPtr->mutable_vals()->Add(valueIndex);
}
