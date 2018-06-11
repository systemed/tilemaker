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

///\brief Specifies callbacks used while loading data using PbfReader
class PbfReaderOutput
{
public:
	///\brief Called when data loading is starting
	virtual void startOsmData() {};

	///\brief Called for every node in input, not just significant nodes
	virtual void everyNode(NodeID id, LatpLon node) {};

	///\brief We are now processing a node
	virtual void setNode(NodeID id, LatpLon node, const std::map<std::string, std::string> &tags) {};

	///\brief We are now processing a way
	virtual void setWay(Way *way, NodeVec *nodeVecPtr, bool inRelation, const std::map<std::string, std::string> &tags) {};

	/** 
	 * \brief We are now processing a relation
	 * (note that we store relations as ways with artificial IDs, and that
	 * we use decrementing positive IDs to give a bit more space for way IDs)
	 */
	virtual void setRelation(Relation *relation, WayVec *outerWayVecPtr, WayVec *innerWayVecPtr,
		const std::map<std::string, std::string> &tags) {};

	///\brief Called when data loading for a single file is done
	virtual void endOsmData() {};
};

/**
 *\brief Reads a PBF OSM file and returns objects as a stream of events to a class derived from PbfReaderOutput
 *
 * The output class is typically OsmMemTiles, which is derived from PbfReaderOutput
 */
class PbfReader
{
public:
	PbfReader();
	virtual ~PbfReader();

	int ReadPbfFile(const std::string &inputFile, std::unordered_set<std::string> &nodeKeys);

	///Pointer to output object. Loaded objects are sent here.
	PbfReaderOutput * output;

private:
	bool ReadNodes(PrimitiveGroup &pg, const std::unordered_set<int> &nodeKeyPositions);

	bool ReadWays(PrimitiveGroup &pg, std::unordered_set<WayID> &waysInRelation);

	bool ReadRelations(PrimitiveGroup &pg);

	void readStringTable(PrimitiveBlock *pbPtr);

	/// Find a string in the dictionary
	int findStringPosition(std::string str);

	// Common tag storage
	std::vector<std::string> stringTable;				// Tag table from the current PrimitiveGroup
	std::map<std::string, uint> tagMap;				// String->position map
};

int ReadPbfBoundingBox(const std::string &inputFile, double &minLon, double &maxLon, 
	double &minLat, double &maxLat, bool &hasClippingBox);

#endif //_READ_PBF_H
