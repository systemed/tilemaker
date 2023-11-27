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

extern const std::string OptionSortTypeThenID;

/**
 *\brief Reads a PBF OSM file and returns objects as a stream of events to a class derived from OsmLuaProcessing
 *
 * The output class is typically OsmMemTiles, which is derived from OsmLuaProcessing
 */
class PbfReader
{
public:	
	enum class ReadPhase { Nodes = 1, Ways = 2, Relations = 4, RelationScan = 8, All = 15 };

	PbfReader(OSMStore &osmStore);

	using pbfreader_generate_output = std::function< std::shared_ptr<OsmLuaProcessing> () >;
	using pbfreader_generate_stream = std::function< std::shared_ptr<std::istream> () >;

	int ReadPbfFile(
		bool hasSortTypeThenID,
		const std::unordered_set<std::string>& nodeKeys,
		unsigned int threadNum,
		const pbfreader_generate_stream& generate_stream,
		const pbfreader_generate_output& generate_output
	);

	// Read tags into a map from a way/node/relation
	using tag_map_t = boost::container::flat_map<std::string, std::string>;
	template<typename T>
	void readTags(T &pbfObject, PrimitiveBlock const &pb, tag_map_t &tags) {
		tags.reserve(pbfObject.keys_size());
		auto keysPtr = pbfObject.mutable_keys();
		auto valsPtr = pbfObject.mutable_vals();
		for (uint n=0; n < pbfObject.keys_size(); n++) {
			tags[pb.stringtable().s(keysPtr->Get(n))] = pb.stringtable().s(valsPtr->Get(n));
		}
	}

private:
	bool ReadBlock(std::istream &infile, OsmLuaProcessing &output, std::size_t datasize, 
	               std::unordered_set<std::string> const &nodeKeys, bool locationsOnWays, ReadPhase phase = ReadPhase::All);
	bool ReadNodes(OsmLuaProcessing &output, PrimitiveGroup &pg, PrimitiveBlock const &pb, const std::unordered_set<int> &nodeKeyPositions);

	bool ReadWays(OsmLuaProcessing &output, PrimitiveGroup &pg, PrimitiveBlock const &pb, bool locationsOnWays);
	bool ScanRelations(OsmLuaProcessing &output, PrimitiveGroup &pg, PrimitiveBlock const &pb);
	bool ReadRelations(OsmLuaProcessing &output, PrimitiveGroup &pg, PrimitiveBlock const &pb);

	inline bool RelationIsType(Relation const &rel, int typeKey, int val) {
		if (typeKey==-1 || val==-1) return false;
		auto typeI = std::find(rel.keys().begin(), rel.keys().end(), typeKey);
		if (typeI==rel.keys().end()) return false;
		int typePos = typeI - rel.keys().begin();
		return rel.vals().Get(typePos) == val;
	}

	/// Find a string in the dictionary
	static int findStringPosition(PrimitiveBlock const &pb, char const *str);
	
	OSMStore &osmStore;
};

int ReadPbfBoundingBox(const std::string &inputFile, double &minLon, double &maxLon, 
	double &minLat, double &maxLat, bool &hasClippingBox);

bool PbfHasOptionalFeature(const std::string& inputFile, const std::string& feature);

#endif //_READ_PBF_H
