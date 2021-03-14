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


///\brief Specifies callbacks used while loading data using PbfReader
class PbfReaderOutput
{
public:
	using tag_map_t = boost::container::flat_map<std::string, std::string>;

	///\brief We are now processing a node
	virtual void setNode(NodeID id, LatpLon node, const tag_map_t &tags) {};

	///\brief We are now processing a way
	virtual void setWay(WayID wayId, OSMStore::handle_t nodeVecHandle, const tag_map_t &tags) {};

	/** 
	 * \brief We are now processing a relation
	 * (note that we store relations as ways with artificial IDs, and that
	 * we use decrementing positive IDs to give a bit more space for way IDs)
	 */
	virtual void setRelation(int64_t relationId, OSMStore::handle_t relationHandle, const tag_map_t &tags) {};
};

///\brief Class to write data to an index file
class PbfIndexWriter
	: public PbfReaderOutput
{
public:
	PbfIndexWriter(OSMStore &osmStore)
		: osmStore(osmStore)
	{ } 

	using tag_map_t = boost::container::flat_map<std::string, std::string>;

	void setNode(NodeID id, LatpLon node, const tag_map_t &tags) override;
	void setWay(WayID wayId, OSMStore::handle_t nodeVecHandle, const tag_map_t &tags) override;
	void setRelation(int64_t relationId, OSMStore::handle_t relationHandle, const tag_map_t &tags) override;

	void save(std::string const &filename);

private:

	OSMStore &osmStore;
};
/**
 *\brief Reads a PBF OSM file and returns objects as a stream of events to a class derived from PbfReaderOutput
 *
 * The output class is typically OsmMemTiles, which is derived from PbfReaderOutput
 */
class PbfReader
{
public:
	PbfReader(OSMStore &osmStore);

	int ReadPbfFile(std::istream &inputFile);

	///Pointer to output object. Loaded objects are sent here.
	PbfReaderOutput * output;

private:
	bool ReadNodes(PrimitiveGroup &pg, PrimitiveBlock const &pb);

	bool ReadWays(PrimitiveGroup &pg, PrimitiveBlock const &pb);

	bool ReadRelations(PrimitiveGroup &pg, PrimitiveBlock const &pb);

	/// Find a string in the dictionary
	static int findStringPosition(PrimitiveBlock const &pb, char const *str);

	using tag_map_t = PbfReaderOutput::tag_map_t;

	struct PbfNodeEntry {
        NodeID nodeId;
        LatpLon node;
        tag_map_t tags;
    };

    struct PbfWayEntry {
        WayID wayId;
        mmap_file_t::handle_t nodeVecHandle;
        tag_map_t tags;
    };

    struct PbfRelationEntry {
        int64_t relationId;
        mmap_file_t::handle_t relationHandle;
        tag_map_t tags;
    };

	std::deque<PbfNodeEntry> node_entries;
	std::deque<PbfWayEntry> way_entries;
	std::deque<PbfRelationEntry> relation_entries;

	OSMStore &osmStore;
};

int ReadPbfBoundingBox(const std::string &inputFile, double &minLon, double &maxLon, 
	double &minLat, double &maxLat, bool &hasClippingBox);

#endif //_READ_PBF_H
