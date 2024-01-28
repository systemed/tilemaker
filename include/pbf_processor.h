/*! \file */ 
#ifndef _READ_PBF_H
#define _READ_PBF_H

#include <atomic>
#include <string>
#include <unordered_set>
#include <vector>
#include <mutex>
#include <map>
#include "osm_store.h"
#include "significant_tags.h"
#include "pbf_reader.h"
#include "tag_map.h"
#include <protozero/data_view.hpp>

class OsmLuaProcessing;

extern const std::string OptionSortTypeThenID;
extern const std::string OptionLocationsOnWays;

struct BlockMetadata {
	long int offset;
	int32_t length;
	bool hasNodes;
	bool hasWays;
	bool hasRelations;

	// We use blocks as the unit of parallelism. Sometimes, a PBF only
	// has a few blocks with relations. In this case, to keep all cores
	// busy, we'll subdivide the block into chunks, and each thread
	// will only process a chunk of the block.
	size_t chunk;
	size_t chunks;
};

struct IndexedBlockMetadata: BlockMetadata {
	size_t index;
};

/**
 *\brief Reads a PBF OSM file and returns objects as a stream of events to a class derived from OsmLuaProcessing
 *
 * The output class is typically OsmMemTiles, which is derived from OsmLuaProcessing
 */
class PbfProcessor
{
public:	
	enum class ReadPhase { Nodes = 1, Ways = 2, Relations = 4, RelationScan = 8, WayScan = 16 };

	PbfProcessor(OSMStore &osmStore);

	using pbfreader_generate_output = std::function< std::shared_ptr<OsmLuaProcessing> () >;
	using pbfreader_generate_stream = std::function< std::shared_ptr<std::istream> () >;

	int ReadPbfFile(
		uint shards,
		bool hasSortTypeThenID,
		const SignificantTags& nodeKeys,
		const SignificantTags& wayKeys,
		unsigned int threadNum,
		const pbfreader_generate_stream& generate_stream,
		const pbfreader_generate_output& generate_output,
		const NodeStore& nodeStore,
		const WayStore& wayStore
	);

	// Read tags into a map from a way/node/relation
	template<typename T>
	void readTags(T &pbfObject, PbfReader::PrimitiveBlock const &pb, TagMap& tags) {
		for (uint n=0; n < pbfObject.keys.size(); n++) {
			auto keyIndex = pbfObject.keys[n];
			auto valueIndex = pbfObject.vals[n];
			tags.addTag(pb.stringTable[keyIndex], pb.stringTable[valueIndex]);
		}
	}

private:
	bool ReadBlock(
		std::istream &infile,
		OsmLuaProcessing &output,
		const BlockMetadata& blockMetadata,
		const SignificantTags& nodeKeys,
		const SignificantTags& wayKeys,
		bool locationsOnWays,
		ReadPhase phase,
		uint shard,
		uint effectiveShard
	);
	bool ReadNodes(OsmLuaProcessing& output, PbfReader::PrimitiveGroup& pg, const PbfReader::PrimitiveBlock& pb, const SignificantTags& nodeKeys);

	bool ReadWays(
		OsmLuaProcessing& output,
		PbfReader::PrimitiveGroup& pg,
		const PbfReader::PrimitiveBlock& pb,
		const SignificantTags& wayKeys,
		bool locationsOnWays,
		uint shard,
		uint effectiveShards
	);
	bool ScanWays(OsmLuaProcessing& output, PbfReader::PrimitiveGroup& pg, const PbfReader::PrimitiveBlock& pb, const SignificantTags& wayKeys);
	bool ScanRelations(OsmLuaProcessing& output, PbfReader::PrimitiveGroup& pg, const PbfReader::PrimitiveBlock& pb, const SignificantTags& wayKeys);
	bool ReadRelations(
		OsmLuaProcessing& output,
		PbfReader::PrimitiveGroup& pg,
		const PbfReader::PrimitiveBlock& pb,
		const BlockMetadata& blockMetadata,
		const SignificantTags& wayKeys,
		uint shard,
		uint effectiveShards
	);

	inline bool relationIsType(const PbfReader::Relation& rel, int typeKey, int val) {
		if (typeKey == -1 || val == -1) return false;
		auto typeI = std::find(rel.keys.begin(), rel.keys.end(), typeKey);
		if (typeI == rel.keys.end()) return false;
		int typePos = typeI - rel.keys.begin();
		return rel.vals[typePos] == val;
	}

	/// Find a string in the dictionary
	static int findStringPosition(const PbfReader::PrimitiveBlock& pb, const std::string& str);
	
	OSMStore &osmStore;
	std::mutex ioMutex;
	std::atomic<bool> compactWarningIssued;
};

int ReadPbfBoundingBox(const std::string &inputFile, double &minLon, double &maxLon, 
	double &minLat, double &maxLat, bool &hasClippingBox);

bool PbfHasOptionalFeature(const std::string& inputFile, const std::string& feature);

#endif //_READ_PBF_H
