/*! \file */ 
#ifndef _READ_PBF_H
#define _READ_PBF_H

#include <string>
#include <unordered_set>
#include <vector>
#include <map>
#include "osm_store.h"

// Protobuf
#include "osmformat.pb.h"
#include "vector_tile.pb.h"

class OsmLuaProcessing;

/**
 *\brief Reads a PBF OSM file and returns objects as a stream of events to a class derived from OsmLuaProcessing
 *
 * The output class is typically OsmMemTiles, which is derived from OsmLuaProcessing
 */
class PbfReader
{
public:	
	enum class ReadPhase { Nodes = 1, Ways = 2, Relations = 4, All = 7 };

	PbfReader(OSMStore &osmStore);

	using pbfreader_generate_output = std::function< std::unique_ptr<OsmLuaProcessing> () >;
	using pbfreader_generate_stream = std::function< std::unique_ptr<std::istream> () >;

	int ReadPbfFile(std::unordered_set<std::string> const &nodeKeys, unsigned int threadNum, 
			pbfreader_generate_stream const &generate_stream,
			pbfreader_generate_output const &generate_output);

private:
	bool ReadBlock(std::istream &infile, OsmLuaProcessing &output, std::pair<std::size_t, std::size_t> progress, std::size_t datasize, std::unordered_set<std::string> const &nodeKeys, ReadPhase phase = ReadPhase::All);
	bool ReadNodes(OsmLuaProcessing &output, PrimitiveGroup &pg, PrimitiveBlock const &pb, const std::unordered_set<int> &nodeKeyPositions);

	bool ReadWays(OsmLuaProcessing &output, PrimitiveGroup &pg, PrimitiveBlock const &pb);

	bool ReadRelations(OsmLuaProcessing &output, PrimitiveGroup &pg, PrimitiveBlock const &pb);

	/// Find a string in the dictionary
	static int findStringPosition(PrimitiveBlock const &pb, char const *str);

	OSMStore &osmStore;
};

int ReadPbfBoundingBox(const std::string &inputFile, double &minLon, double &maxLon, 
	double &minLat, double &maxLat, bool &hasClippingBox);

#endif //_READ_PBF_H
