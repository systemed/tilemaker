#include <iostream>
#include "read_pbf.h"
#include "pbf_blocks.h"

#include <boost/interprocess/streams/bufferstream.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/post.hpp>
#include <unordered_set>

#include "node_store.h"
#include "way_store.h"
#include "osm_lua_processing.h"
#include "mmap_allocator.h"

using namespace std;

const std::string OptionSortTypeThenID = "Sort.Type_then_ID";
const std::string OptionLocationsOnWays = "LocationsOnWays";
std::atomic<uint64_t> blocksProcessed(0), blocksToProcess(0);

PbfReader::PbfReader(OSMStore &osmStore)
	: osmStore(osmStore)
{ }

bool PbfReader::ReadNodes(OsmLuaProcessing &output, PrimitiveGroup &pg, PrimitiveBlock const &pb, const unordered_set<int> &nodeKeyPositions)
{
	// ----	Read nodes

	if (pg.has_dense()) {
		int64_t nodeId  = 0;
		int lon = 0;
		int lat = 0;
		int kvPos = 0;
		DenseNodes dense = pg.dense();

		std::vector<NodeStore::element_t> nodes;		
		for (int j=0; j<dense.id_size(); j++) {
			nodeId += dense.id(j);
			lon    += dense.lon(j);
			lat    += dense.lat(j);
			LatpLon node = { int(lat2latp(double(lat)/10000000.0)*10000000.0), lon };

			bool significant = false;
			int kvStart = kvPos;
			if (dense.keys_vals_size()>0) {
				while (dense.keys_vals(kvPos)>0) {
					if (nodeKeyPositions.find(dense.keys_vals(kvPos)) != nodeKeyPositions.end()) {
						significant = true;
					}
					kvPos+=2;
				}
				kvPos++;
			}

			nodes.push_back(std::make_pair(static_cast<NodeID>(nodeId), node));

			if (significant) {
				// For tagged nodes, call Lua, then save the OutputObject
				boost::container::flat_map<std::string, std::string> tags;
				tags.reserve(kvPos / 2);

				for (uint n=kvStart; n<kvPos-1; n+=2) {
					tags[pb.stringtable().s(dense.keys_vals(n))] = pb.stringtable().s(dense.keys_vals(n+1));
				}
				output.setNode(static_cast<NodeID>(nodeId), node, tags);
			} 

		}

		osmStore.nodes.insert(nodes);
		return true;
	}
	return false;
}

bool PbfReader::ReadWays(
	OsmLuaProcessing &output,
	PrimitiveGroup &pg,
	PrimitiveBlock const &pb,
	bool locationsOnWays,
	uint shard,
	uint effectiveShards
) {
	// ----	Read ways

	if (pg.ways_size() > 0) {
		Way pbfWay;

		const bool wayStoreRequiresNodes = osmStore.ways.requiresNodes();

		std::vector<WayStore::ll_element_t> llWays;
		std::vector<std::pair<WayID, std::vector<NodeID>>> nodeWays;
		LatpLonVec llVec;
		std::vector<NodeID> nodeVec;

		for (int j=0; j<pg.ways_size(); j++) {
			llVec.clear();
			nodeVec.clear();

			pbfWay = pg.ways(j);
			WayID wayId = static_cast<WayID>(pbfWay.id());
			if (wayId >= pow(2,42)) throw std::runtime_error("Way ID negative or too large: "+std::to_string(wayId));

			// Assemble nodelist
			if (locationsOnWays) {
				int lat=0, lon=0;
				llVec.reserve(pbfWay.lats_size());
				for (int k=0; k<pbfWay.lats_size(); k++) {
					lat += pbfWay.lats(k);
					lon += pbfWay.lons(k);
					LatpLon ll = { int(lat2latp(double(lat)/10000000.0)*10000000.0), lon };
					llVec.push_back(ll);
				}
			} else {
				int64_t nodeId = 0;
				llVec.reserve(pbfWay.refs_size());
				nodeVec.reserve(pbfWay.refs_size());

				bool skipToNext = false;

				for (int k=0; k<pbfWay.refs_size(); k++) {
					nodeId += pbfWay.refs(k);

					if (k == 0 && effectiveShards > 1 && !osmStore.nodes.contains(shard, nodeId)) {
						skipToNext = true;
						break;
					}

					try {
						llVec.push_back(osmStore.nodes.at(static_cast<NodeID>(nodeId)));
						nodeVec.push_back(nodeId);
					} catch (std::out_of_range &err) {
						if (osmStore.integrity_enforced()) throw err;
					}
				}

				if (skipToNext)
					continue;
			}
			if (llVec.empty()) continue;

			try {
				tag_map_t tags;
				readTags(pbfWay, pb, tags);
				bool emitted = output.setWay(static_cast<WayID>(pbfWay.id()), llVec, tags);

				// If we need it for later, store the way's coordinates in the global way store
				if (emitted || osmStore.way_is_used(wayId)) {
					if (wayStoreRequiresNodes)
						nodeWays.push_back(std::make_pair(wayId, nodeVec));
					else
						llWays.push_back(std::make_pair(wayId, WayStore::latplon_vector_t(llVec.begin(), llVec.end())));
				}

			} catch (std::out_of_range &err) {
				// Way is missing a node?
				cerr << endl << err.what() << endl;
			}

		}

		if (wayStoreRequiresNodes) {
			osmStore.ways.shard(shard).insertNodes(nodeWays);
		} else {
			osmStore.ways.shard(shard).insertLatpLons(llWays);
		}

		return true;
	}
	return false;
}

bool PbfReader::ScanRelations(OsmLuaProcessing &output, PrimitiveGroup &pg, PrimitiveBlock const &pb) {
	// Scan relations to see which ways we need to save
	if (pg.relations_size()==0) return false;

	int typeKey = findStringPosition(pb, "type");
	int mpKey   = findStringPosition(pb, "multipolygon");

	for (int j=0; j<pg.relations_size(); j++) {
		Relation pbfRelation = pg.relations(j);
		bool isMultiPolygon = RelationIsType(pbfRelation, typeKey, mpKey);
		bool isAccepted = false;
		WayID relid = static_cast<WayID>(pbfRelation.id());
		if (!isMultiPolygon) {
			if (output.canReadRelations()) {
				tag_map_t tags;
				readTags(pbfRelation, pb, tags);
				isAccepted = output.scanRelation(relid, tags);
			}
			if (!isAccepted) continue;
		}
		int64_t lastID = 0;
		for (int n=0; n < pbfRelation.memids_size(); n++) {
			lastID += pbfRelation.memids(n);
			if (pbfRelation.types(n) != Relation_MemberType_WAY) { continue; }
			if (lastID >= pow(2,42)) throw std::runtime_error("Way ID in relation "+std::to_string(relid)+" negative or too large: "+std::to_string(lastID));
			osmStore.mark_way_used(static_cast<WayID>(lastID));
			if (isAccepted) { osmStore.relation_contains_way(relid, lastID); }
		}
	}
	return true;
}

bool PbfReader::ReadRelations(
	OsmLuaProcessing& output,
	PrimitiveGroup& pg,
	const PrimitiveBlock& pb,
	const BlockMetadata& blockMetadata
) {
	// ----	Read relations

	if (pg.relations_size() > 0) {
		std::vector<RelationStore::element_t> relations;

		int typeKey = findStringPosition(pb, "type");
		int mpKey   = findStringPosition(pb, "multipolygon");
		int boundaryKey = findStringPosition(pb, "boundary");
		int innerKey= findStringPosition(pb, "inner");
		int outerKey= findStringPosition(pb, "outer");
		if (typeKey >-1 && mpKey>-1) {
			for (int j=0; j<pg.relations_size(); j++) {
				if (j % blockMetadata.chunks != blockMetadata.chunk)
					continue;

				Relation pbfRelation = pg.relations(j);
				bool isMultiPolygon = RelationIsType(pbfRelation, typeKey, mpKey);
				bool isBoundary = RelationIsType(pbfRelation, typeKey, boundaryKey);
				if (!isMultiPolygon && !isBoundary && !output.canWriteRelations()) continue;

				// Read relation members
				WayVec outerWayVec, innerWayVec;
				int64_t lastID = 0;
				bool isInnerOuter = isBoundary || isMultiPolygon;
				for (int n=0; n < pbfRelation.memids_size(); n++) {
					lastID += pbfRelation.memids(n);
					if (pbfRelation.types(n) != Relation_MemberType_WAY) { continue; }
					int32_t role = pbfRelation.roles_sid(n);
					if (role==innerKey || role==outerKey) isInnerOuter=true;
					WayID wayId = static_cast<WayID>(lastID);
					(role == innerKey ? innerWayVec : outerWayVec).push_back(wayId);
				}

				try {
					tag_map_t tags;
					readTags(pbfRelation, pb, tags);
					output.setRelation(pbfRelation.id(), outerWayVec, innerWayVec, tags, isMultiPolygon, isInnerOuter);

				} catch (std::out_of_range &err) {
					// Relation is missing a member?
					cerr << endl << err.what() << endl;
				}
			}
		}

		osmStore.relations_insert_front(relations);
		return true;
	}
	return false;
}

// Returns true when block was completely handled, thus could be omited by another phases.
bool PbfReader::ReadBlock(
	std::istream& infile,
	OsmLuaProcessing& output,
	const BlockMetadata& blockMetadata,
	const unordered_set<string>& nodeKeys,
	bool locationsOnWays,
	ReadPhase phase,
	uint shard,
	uint effectiveShards
) 
{
	infile.seekg(blockMetadata.offset);

	PrimitiveBlock pb;
	readBlock(&pb, blockMetadata.length, infile);
	if (infile.eof()) {
		return true;
	}

	// Keep count of groups read during this phase.
	std::size_t read_groups = 0;

	// Read the string table, and pre-calculate the positions of valid node keys
	unordered_set<int> nodeKeyPositions;
	for (auto it : nodeKeys) {
		nodeKeyPositions.insert(findStringPosition(pb, it.c_str()));
	}

	for (int i=0; i<pb.primitivegroup_size(); i++) {
		PrimitiveGroup pg;
		pg = pb.primitivegroup(i);
	
		auto output_progress = [&]()
		{
			if (ioMutex.try_lock()) {
				std::ostringstream str;
				str << "\r";
				void_mmap_allocator::reportStoreSize(str);
				if (effectiveShards > 1)
					str << std::to_string(shard + 1) << "/" << std::to_string(effectiveShards) << " ";

				str << "Block " << blocksProcessed.load() << "/" << blocksToProcess.load() << " ways " << pg.ways_size() << " relations " << pg.relations_size() << "                  ";
				std::cout << str.str();
				std::cout.flush();
				ioMutex.unlock();
			}
		};

		if(phase == ReadPhase::Nodes) {
			bool done = ReadNodes(output, pg, pb, nodeKeyPositions);
			if(done) { 
				output_progress();
				++read_groups;
				continue;
			}
		}

		if(phase == ReadPhase::RelationScan) {
			osmStore.ensureUsedWaysInited();
			bool done = ScanRelations(output, pg, pb);
			if(done) { 
				if (ioMutex.try_lock()) {
					std::cout << "\r(Scanning for ways used in relations: " << (100*blocksProcessed.load()/blocksToProcess.load()) << "%)           ";
					std::cout.flush();
					ioMutex.unlock();
				}
				continue;
			}
		}
	
		if(phase == ReadPhase::Ways) {
			bool done = ReadWays(output, pg, pb, locationsOnWays, shard, effectiveShards);
			if(done) { 
				output_progress();
				++read_groups;
				continue;
			}
		}

		if(phase == ReadPhase::Relations) {
			bool done = ReadRelations(output, pg, pb, blockMetadata);
			if(done) { 
				output_progress();
				++read_groups;
				continue;
			}
		}
	}

	// Possible cases of a block contents:
	// - single group
	// - multiple groups of the same type
	// - multiple groups of the different type
	// 
	// In later case block would not be handled during this phase, and should be
	// read again in remaining phases. Thus we return false to indicate that the
	// block was not handled completelly.
	if(read_groups != pb.primitivegroup_size()) {
		return false;
	}

	// We can only delete blocks if we're confident we've processed everything,
	// which is not possible in the case of subdivided blocks.
	return (shard + 1 == effectiveShards) && blockMetadata.chunks == 1;
}

bool blockHasPrimitiveGroupSatisfying(
	std::istream& infile,
	const BlockMetadata block,
	std::function<bool(const PrimitiveGroup&)> test
) {
	PrimitiveBlock pb;

	// We may have previously read to EOF, so clear the internal error state
	infile.clear();
	infile.seekg(block.offset);
	readBlock(&pb, block.length, infile);
	if (infile.eof()) {
		throw std::runtime_error("blockHasPrimitiveGroupSatisfying got unexpected eof");
	}

	for (int i=0; i<pb.primitivegroup_size(); i++) {
		PrimitiveGroup pg;
		pg = pb.primitivegroup(i);

		if (test(pg))
			return false;
	}
	
	return true;
}

int PbfReader::ReadPbfFile(
	uint shards,
	bool hasSortTypeThenID,
	unordered_set<string> const& nodeKeys,
	unsigned int threadNum,
	const pbfreader_generate_stream& generate_stream,
	const pbfreader_generate_output& generate_output,
	const NodeStore& nodeStore,
	const WayStore& wayStore
)
{
	auto infile = generate_stream();

	// ----	Read PBF
	osmStore.clear();

	HeaderBlock block;
	readBlock(&block, readHeader(*infile).datasize(), *infile);
	bool locationsOnWays = false;
	for (std::string option : block.optional_features()) {
		if (option == OptionLocationsOnWays) {
			std::cout << ".osm.pbf file has locations on ways" << std::endl;
			locationsOnWays = true;
		}
	}

	std::map<std::size_t, BlockMetadata> blocks;

	// Track the filesize - note that we can't rely on tellg(), as
	// its meant to be an opaque token useful only for seeking.
	size_t filesize = 0;
	while (true) {
		BlobHeader bh = readHeader(*infile);
		filesize += bh.datasize();
		if (infile->eof()) {
			break;
		}

		blocks[blocks.size()] = { (long int)infile->tellg(), bh.datasize(), true, true, true, 0, 1 };
		infile->seekg(bh.datasize(), std::ios_base::cur);
	}

	if (hasSortTypeThenID) {
		// The PBF's blocks are sorted by type, then ID. We can do a binary search
		// to learn where the blocks transition between object types, which
		// enables a more efficient partitioning of work for reading.
		std::vector<size_t> indexes;
		for (int i = 0; i < blocks.size(); i++)
			indexes.push_back(i);

		const auto& waysStart = std::lower_bound(
			indexes.begin(),
			indexes.end(),
			0,
			[&blocks, &infile](const auto &i, const auto &ignored) {
				return blockHasPrimitiveGroupSatisfying(
					*infile,
					blocks[i],
					[](const PrimitiveGroup&pg) { return pg.ways_size() > 0 || pg.relations_size() > 0; }
				);
			}
		);

		const auto& relationsStart = std::lower_bound(
			indexes.begin(),
			indexes.end(),
			0,
			[&blocks, &infile](const auto &i, const auto &ignored) {
				return blockHasPrimitiveGroupSatisfying(
					*infile,
					blocks[i],
					[](const PrimitiveGroup&pg) { return pg.relations_size() > 0; }
				);
			}
		);

		for (auto it = indexes.begin(); it != indexes.end(); it++) {
			blocks[*it].hasNodes = it <= waysStart;
			blocks[*it].hasWays = it >= waysStart && it <= relationsStart;
			blocks[*it].hasRelations = it >= relationsStart;
		}
	}


	// PBFs generated by Osmium have 8,000 entities per block,
	// and each block is about 64KB.
	//
	// PBFs generated by osmconvert (e.g., BBBike PBFs) have as
	// many entities as fit in 31MB. Each block is about 16MB.
	//
	// Osmium PBFs seem to be processed about 3x faster than osmconvert
	// PBFs, so try to hint to the user when they could speed up their
	// pipeline.
	if (filesize / blocks.size() > 1000000) {
		std::cout << "warning: PBF has very large blocks, which may slow processing" << std::endl;
		std::cout << "         to fix: osmium cat -f pbf your-file.osm.pbf -o optimized.osm.pbf" << std::endl;
	}


	std::vector<ReadPhase> all_phases = { ReadPhase::Nodes, ReadPhase::RelationScan, ReadPhase::Ways, ReadPhase::Relations };
	for(auto phase: all_phases) {
		uint effectiveShards = 1;

		// On memory-constrained machines, we might read ways multiple times in order
		// to keep the working set of nodes limited.
		if (phase == ReadPhase::Ways)
			effectiveShards = shards;

		for (int shard = 0; shard < effectiveShards; shard++) {
			// If we're in ReadPhase::Ways, only do a pass if there is at least one
			// entry in the pass's shard.
			if (phase == ReadPhase::Ways && nodeStore.shard(shard).size() == 0)
				continue;

#ifdef CLOCK_MONOTONIC
			timespec start, end;
			clock_gettime(CLOCK_MONOTONIC, &start);
#endif

			// Launch the pool with threadNum threads
			boost::asio::thread_pool pool(threadNum);
			std::mutex block_mutex;

			// If we're in ReadPhase::Relations and there aren't many blocks left
			// to read, increase parallelism by letting each thread only process
			// a portion of the block.
			if (phase == ReadPhase::Relations && blocks.size() < threadNum * 2) {
				std::cout << "only " << blocks.size() << " relation blocks; subdividing for better parallelism" << std::endl;
				std::map<std::size_t, BlockMetadata> moreBlocks;
				for (const auto& block : blocks) {
					BlockMetadata newBlock = block.second;
					newBlock.chunks = threadNum;
					for (size_t i = 0; i < threadNum; i++) {
						newBlock.chunk = i;
						moreBlocks[moreBlocks.size()] = newBlock;
					}
				}
				blocks = moreBlocks;
			}

			std::deque<std::vector<IndexedBlockMetadata>> blockRanges;
			std::map<std::size_t, BlockMetadata> filteredBlocks;
			for (const auto& entry : blocks) {
				if ((phase == ReadPhase::Nodes && entry.second.hasNodes) ||
						(phase == ReadPhase::RelationScan && entry.second.hasRelations) ||
						(phase == ReadPhase::Ways && entry.second.hasWays) ||
						(phase == ReadPhase::Relations && entry.second.hasRelations))
					filteredBlocks[entry.first] = entry.second;
			}

			blocksToProcess = filteredBlocks.size();
			blocksProcessed = 0;

			// When processing blocks, we try to give each worker large batches
			// of contiguous blocks, so that they might benefit from long runs
			// of sorted indexes, and locality of nearby IDs.
			const size_t batchSize = (filteredBlocks.size() / (threadNum * 8)) + 1;

			size_t consumed = 0;
			auto it = filteredBlocks.begin();
			while(it != filteredBlocks.end()) {
				std::vector<IndexedBlockMetadata> blockRange;
				blockRange.reserve(batchSize);
				size_t max = consumed + batchSize;
				for (; consumed < max && it != filteredBlocks.end(); consumed++) {
					IndexedBlockMetadata ibm;
					memcpy(&ibm, &it->second, sizeof(BlockMetadata));
					ibm.index = it->first;
					blockRange.push_back(ibm);
					it++;
				}
				blockRanges.push_back(blockRange);
			}

			{
				for(const std::vector<IndexedBlockMetadata>& blockRange: blockRanges) {
					boost::asio::post(pool, [=, &blockRange, &blocks, &block_mutex, &nodeKeys]() {
						if (phase == ReadPhase::Nodes)
							osmStore.nodes.batchStart();
						if (phase == ReadPhase::Ways)
							osmStore.ways.batchStart();

						for (const IndexedBlockMetadata& indexedBlockMetadata: blockRange) {
							auto infile = generate_stream();
							auto output = generate_output();

							if(ReadBlock(*infile, *output, indexedBlockMetadata, nodeKeys, locationsOnWays, phase, shard, effectiveShards)) {
								const std::lock_guard<std::mutex> lock(block_mutex);
								blocks.erase(indexedBlockMetadata.index);	
							}
							blocksProcessed++;
						}
					});
				}
			}
		
			pool.join();

#ifdef CLOCK_MONOTONIC
			clock_gettime(CLOCK_MONOTONIC, &end);
			uint64_t elapsedNs = 1e9 * (end.tv_sec - start.tv_sec) + end.tv_nsec - start.tv_nsec;
			std::cout << "(" << std::to_string((uint32_t)(elapsedNs / 1e6)) << " ms)" << std::endl;
#endif
		}

		if(phase == ReadPhase::Nodes) {
			osmStore.nodes.finalize(threadNum);
		}
		if(phase == ReadPhase::Ways) {
			osmStore.ways.finalize(threadNum);
		}
	}
	return 0;
}

// Find a string in the dictionary
int PbfReader::findStringPosition(PrimitiveBlock const &pb, char const *str) {
	for (int i=0; i<pb.stringtable().s_size(); i++) {
		if(pb.stringtable().s(i) == str)
			return i;
	}
	return -1;
}


// *************************************************

int ReadPbfBoundingBox(const std::string &inputFile, double &minLon, double &maxLon, 
	double &minLat, double &maxLat, bool &hasClippingBox)
{
	fstream infile(inputFile, ios::in | ios::binary);
	if (!infile) { cerr << "Couldn't open .pbf file " << inputFile << endl; return -1; }
	HeaderBlock block;
	readBlock(&block, readHeader(infile).datasize(), infile);
	if (block.has_bbox()) {
		hasClippingBox = true;		
		minLon = block.bbox().left()  /1000000000.0;
		maxLon = block.bbox().right() /1000000000.0;
		minLat = block.bbox().bottom()/1000000000.0;
		maxLat = block.bbox().top()   /1000000000.0;
	}
	infile.close();
	return 0;
}

bool PbfHasOptionalFeature(const std::string& inputFile, const std::string& feature) {
	fstream infile(inputFile, ios::in | ios::binary);
	if (!infile) { cerr << "Couldn't open .pbf file " << inputFile << endl; return -1; }
	HeaderBlock block;
	readBlock(&block, readHeader(infile).datasize(), infile);

	for (const std::string& option: block.optional_features())
		if (option == feature)
			return true;

	return false;
}
