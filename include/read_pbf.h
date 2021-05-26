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

#include <boost/container/flat_map.hpp>

class OsmLuaProcessing;

///\brief Specifies callbacks used while loading data using PbfReader
class PbfReaderOutput
{
public:
	using tag_map_t = boost::container::flat_map<std::string, std::string>;

	///\brief We are now processing a node
	virtual void setNode(NodeID id, LatpLon node, const tag_map_t &tags) {};

	///\brief We are now processing a way
	virtual void setWay(WayID wayId, NodeVec const &nodeVec, const tag_map_t &tags) {};

	/** 
	 * \brief We are now processing a relation
	 * (note that we store relations as ways with artificial IDs, and that
	 * we use decrementing positive IDs to give a bit more space for way IDs)
	 */
	virtual void setRelation(int64_t relationId, WayVec const &outerWayVec, WayVec const &innerWayVec, const tag_map_t &tags) { }
};


/**
 *\brief Reads a PBF OSM file and returns objects as a stream of events to a class derived from PbfReaderOutput
 *
 * The output class is typically OsmMemTiles, which is derived from PbfReaderOutput
 */
class PbfReader
{
public:	
	enum class ReadPhase { Nodes = 1, Ways = 2, Relations = 4, All = 7 };

	PbfReader(OSMStore &osmStore);

	using pbfreader_generate_output = std::function< std::unique_ptr<PbfReaderOutput> () >;
	int ReadPbfFile(std::string const &filename, std::unordered_set<std::string> const &nodeKeys, unsigned int threadNum, pbfreader_generate_output const &generate_output);
	int ReadPbfFile(std::istream &inputFile, std::unordered_set<std::string> const &nodeKeys, PbfReaderOutput &output);

private:
	bool ReadBlock(std::istream &infile, PbfReaderOutput &output, std::pair<std::size_t, std::size_t> progress, std::size_t datasize, std::unordered_set<std::string> const &nodeKeys, ReadPhase phase = ReadPhase::All);
	bool ReadNodes(PbfReaderOutput &output, PrimitiveGroup &pg, PrimitiveBlock const &pb, const std::unordered_set<int> &nodeKeyPositions);

	bool ReadWays(PbfReaderOutput &output, PrimitiveGroup &pg, PrimitiveBlock const &pb);

	bool ReadRelations(PbfReaderOutput &output, PrimitiveGroup &pg, PrimitiveBlock const &pb);

	/// Find a string in the dictionary
	static int findStringPosition(PrimitiveBlock const &pb, char const *str);

	using tag_map_t = PbfReaderOutput::tag_map_t;

	OSMStore &osmStore;
};

int ReadPbfBoundingBox(const std::string &inputFile, double &minLon, double &maxLon, 
	double &minLat, double &maxLat, bool &hasClippingBox);

#endif //_READ_PBF_H
