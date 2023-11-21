#include <iostream>
#include "read_pbf.h"
#include "pbf_blocks.h"

#include <boost/interprocess/streams/bufferstream.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/post.hpp>
#include <unordered_set>

#include "osm_lua_processing.h"

using namespace std;

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

		osmStore.nodes_insert_back(nodes);
		return true;
	}
	return false;
}

bool PbfReader::ReadWays(OsmLuaProcessing &output, PrimitiveGroup &pg, PrimitiveBlock const &pb, bool locationsOnWays) {
	// ----	Read ways

	if (pg.ways_size() > 0) {
		Way pbfWay;

		std::vector<WayStore::element_t> ways;

		for (int j=0; j<pg.ways_size(); j++) {
			pbfWay = pg.ways(j);
			WayID wayId = static_cast<WayID>(pbfWay.id());
			if (wayId >= pow(2,42)) throw std::runtime_error("Way ID negative or too large: "+std::to_string(wayId));

			// Assemble nodelist
			LatpLonVec llVec;
			if (locationsOnWays) {
				int lat=0, lon=0;
				for (int k=0; k<pbfWay.lats_size(); k++) {
					lat += pbfWay.lats(k);
					lon += pbfWay.lons(k);
					LatpLon ll = { int(lat2latp(double(lat)/10000000.0)*10000000.0), lon };
					llVec.push_back(ll);
				}
			} else {
				int64_t nodeId = 0;
				for (int k=0; k<pbfWay.refs_size(); k++) {
					nodeId += pbfWay.refs(k);
					try {
						llVec.push_back(osmStore.nodes_at(static_cast<NodeID>(nodeId)));
					} catch (std::out_of_range &err) {
						if (osmStore.integrity_enforced()) throw err;
					}
				}
			}
			if (llVec.empty()) continue;

			try {
				tag_map_t tags;
				readTags(pbfWay, pb, tags);

				// If we need it for later, store the way's coordinates in the global way store
				if (osmStore.way_is_used(wayId)) {
					ways.push_back(std::make_pair(wayId, WayStore::latplon_vector_t(llVec.begin(), llVec.end())));
				}
				output.setWay(static_cast<WayID>(pbfWay.id()), llVec, tags);

			} catch (std::out_of_range &err) {
				// Way is missing a node?
				cerr << endl << err.what() << endl;
			}

		}

		osmStore.ways_insert_back(ways);
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

bool PbfReader::ReadRelations(OsmLuaProcessing &output, PrimitiveGroup &pg, PrimitiveBlock const &pb) {
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
bool PbfReader::ReadBlock(std::istream &infile, OsmLuaProcessing &output, std::pair<std::size_t, std::size_t> progress, std::size_t datasize, 
                          unordered_set<string> const &nodeKeys, bool locationsOnWays, ReadPhase phase) 
{
	PrimitiveBlock pb;
	readBlock(&pb, datasize, infile);
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
			std::ostringstream str;
			osmStore.reportStoreSize(str);
			str << "Block " << progress.first << "/" << progress.second << " ways " << pg.ways_size() << " relations " << pg.relations_size() << "        \r";
			std::cout << str.str();
			std::cout.flush();
		};

		if(phase == ReadPhase::Nodes || phase == ReadPhase::All) {
			bool done = ReadNodes(output, pg, pb, nodeKeyPositions);
			if(done) { 
				output_progress();
				++read_groups;
				continue;
			}
		}

		if(phase == ReadPhase::RelationScan || phase == ReadPhase::All) {
			osmStore.ensure_used_ways_inited();
			bool done = ScanRelations(output, pg, pb);
			if(done) { 
				std::cout << "(Scanning for ways used in relations: " << (100*progress.first/progress.second) << "%)\r";
				std::cout.flush();
				continue;
			}
		}
	
		if(phase == ReadPhase::Ways || phase == ReadPhase::All) {
			bool done = ReadWays(output, pg, pb, locationsOnWays);
			if(done) { 
				output_progress();
				++read_groups;
				continue;
			}
		}

		if(phase == ReadPhase::Relations || phase == ReadPhase::All) {
			bool done = ReadRelations(output, pg, pb);
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

	return true;
}

int PbfReader::ReadPbfFile(unordered_set<string> const &nodeKeys, unsigned int threadNum, 
		pbfreader_generate_stream const &generate_stream, pbfreader_generate_output const &generate_output)
{
	auto infile = generate_stream();

	// ----	Read PBF
	osmStore.clear();

	HeaderBlock block;
	readBlock(&block, readHeader(*infile).datasize(), *infile);
	bool locationsOnWays = false;
	for (std::string option : block.optional_features()) {
		if (option=="LocationsOnWays") {
			std::cout << ".osm.pbf file has locations on ways" << std::endl;
			locationsOnWays = true;
		}
	}

	std::map<std::size_t, std::pair< std::size_t, std::size_t> > blocks;

	while (true) {
		BlobHeader bh = readHeader(*infile);
		if (infile->eof()) {
			break;
		}

		blocks[blocks.size()] = std::make_pair(infile->tellg(), bh.datasize());
		infile->seekg(bh.datasize(), std::ios_base::cur);
		
	}


	std::mutex block_mutex;

	std::size_t total_blocks = blocks.size();

	std::vector<ReadPhase> all_phases = { ReadPhase::Nodes, ReadPhase::RelationScan, ReadPhase::Ways, ReadPhase::Relations };
	for(auto phase: all_phases) {
		// Launch the pool with threadNum threads
		boost::asio::thread_pool pool(threadNum);

		{
			const std::lock_guard<std::mutex> lock(block_mutex);
			for(auto const &block: blocks) {
				boost::asio::post(pool, [=, progress=std::make_pair(block.first, total_blocks), block=block.second, &blocks, &block_mutex, &nodeKeys]() {
					auto infile = generate_stream();
					auto output = generate_output();

					infile->seekg(block.first);
					if(ReadBlock(*infile, *output, progress, block.second, nodeKeys, locationsOnWays, phase)) {
						const std::lock_guard<std::mutex> lock(block_mutex);
						blocks.erase(progress.first);	
					}
				});
			}
		}
	
		pool.join();

		if(phase == ReadPhase::Nodes) {
			osmStore.nodes_sort(threadNum);
		}
		if(phase == ReadPhase::Ways) {
			osmStore.ways_sort(threadNum);
		}
	}
	const auto& osmLuaProcessing = generate_output();
	osmLuaProcessing->FlushTileIndex();
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

