#include <iostream>
#include "read_pbf.h"
#include "pbf_blocks.h"

#include <boost/interprocess/streams/bufferstream.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/post.hpp>

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
			// For tagged nodes, call Lua, then save the OutputObject
			boost::container::flat_map<std::string, std::string> tags;

			nodes.push_back(std::make_pair(static_cast<NodeID>(nodeId), node));

			if (significant) {
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

bool PbfReader::ReadWays(OsmLuaProcessing &output, PrimitiveGroup &pg, PrimitiveBlock const &pb) {
	// ----	Read ways

	if (pg.ways_size() > 0) {
		Way pbfWay;

		std::vector<WayStore::element_t> ways;

		for (int j=0; j<pg.ways_size(); j++) {
			pbfWay = pg.ways(j);
			WayID wayId = static_cast<WayID>(pbfWay.id());

			// Assemble nodelist
			int64_t nodeId = 0;
			NodeVec nodeVec;
			for (int k=0; k<pbfWay.refs_size(); k++) {
				nodeId += pbfWay.refs(k);
				nodeVec.push_back(static_cast<NodeID>(nodeId));
			}

			try {
				auto keysPtr = pbfWay.mutable_keys();
				auto valsPtr = pbfWay.mutable_vals();
				boost::container::flat_map<std::string, std::string> tags;
				for (uint n=0; n < pbfWay.keys_size(); n++) {
					tags[pb.stringtable().s(keysPtr->Get(n))] = pb.stringtable().s(valsPtr->Get(n));
				}

				// Store the way's nodes in the global way store
				ways.push_back(std::make_pair(static_cast<WayID>(pbfWay.id()), 
					WayStore::nodeid_vector_t(nodeVec.begin(), nodeVec.end())));
				output.setWay(static_cast<WayID>(pbfWay.id()), nodeVec, tags);

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

bool PbfReader::ReadRelations(OsmLuaProcessing &output, PrimitiveGroup &pg, PrimitiveBlock const &pb) {
	// ----	Read relations
	//		(just multipolygons for now; we should do routes in time)

	if (pg.relations_size() > 0) {
		std::vector<RelationStore::element_t> relations;

		int typeKey = findStringPosition(pb, "type");
		int mpKey   = findStringPosition(pb, "multipolygon");
		int innerKey= findStringPosition(pb, "inner");
		//int outerKey= findStringPosition(pb, "outer");
		if (typeKey >-1 && mpKey>-1) {
			for (int j=0; j<pg.relations_size(); j++) {
				Relation pbfRelation = pg.relations(j);
				if (find(pbfRelation.keys().begin(), pbfRelation.keys().end(), typeKey) == pbfRelation.keys().end()) { continue; }
				if (find(pbfRelation.vals().begin(), pbfRelation.vals().end(), mpKey  ) == pbfRelation.vals().end()) { continue; }

				// Read relation members
				WayVec outerWayVec, innerWayVec;
				int64_t lastID = 0;
				for (int n=0; n < pbfRelation.memids_size(); n++) {
					lastID += pbfRelation.memids(n);
					if (pbfRelation.types(n) != Relation_MemberType_WAY) { continue; }
					int32_t role = pbfRelation.roles_sid(n);
					// if (role != innerKey && role != outerKey) { continue; }
					// ^^^^ commented out so that we don't die horribly when a relation has no outer way
					WayID wayId = static_cast<WayID>(lastID);
					(role == innerKey ? innerWayVec : outerWayVec).push_back(wayId);
				}

				try {
					auto keysPtr = pbfRelation.mutable_keys();
					auto valsPtr = pbfRelation.mutable_vals();
					boost::container::flat_map<std::string, std::string> tags;
					for (uint n=0; n < pbfRelation.keys_size(); n++) {
						tags[pb.stringtable().s(keysPtr->Get(n))] = pb.stringtable().s(valsPtr->Get(n));

					}

					// Store the relation members in the global relation store
					relations.push_back(std::make_pair(pbfRelation.id(), 
						std::make_pair(
							RelationStore::wayid_vector_t(outerWayVec.begin(), outerWayVec.end()),
							RelationStore::wayid_vector_t(innerWayVec.begin(), innerWayVec.end()))));

					output.setRelation(pbfRelation.id(), outerWayVec, innerWayVec, tags);

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

bool PbfReader::ReadBlock(std::istream &infile, OsmLuaProcessing &output, std::pair<std::size_t, std::size_t> progress, std::size_t datasize, unordered_set<string> const &nodeKeys, ReadPhase phase) 
{
	PrimitiveBlock pb;
	readBlock(&pb, datasize, infile);
	if (infile.eof()) {
		return true;
	}

	bool handled_block = false;

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
				handled_block = true;
				continue;
			}
		}
	
		if(phase == ReadPhase::Ways || phase == ReadPhase::All) {
			bool done = ReadWays(output, pg, pb);
			if(done) { 
				output_progress();
				handled_block = true;
				continue;
			}
		}

		if(phase == ReadPhase::Relations || phase == ReadPhase::All) {
			bool done = ReadRelations(output, pg, pb);
			if(done) { 
				output_progress();
				handled_block = true;
				continue;
			}
		}
	}

	return handled_block;
}

int PbfReader::ReadPbfFile(unordered_set<string> const &nodeKeys, unsigned int threadNum, 
		pbfreader_generate_stream const &generate_stream, pbfreader_generate_output const &generate_output)
{
	auto infile = generate_stream();

	// ----	Read PBF
	osmStore.clear();

	HeaderBlock block;
	readBlock(&block, readHeader(*infile).datasize(), *infile);

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

	std::vector<ReadPhase> all_phases = { ReadPhase::Nodes, ReadPhase::Ways, ReadPhase::Relations };
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
					if(ReadBlock(*infile, *output, progress, block.second, nodeKeys, phase)) {
						const std::lock_guard<std::mutex> lock(block_mutex);
						blocks.erase(progress.first);	
					}
				});
			}
		}
	
		pool.join();

		if(phase == ReadPhase::Nodes) {
			std::cout << "\nSorting nodes" << std::endl;
			osmStore.nodes_sort(threadNum);
		}
		if(phase == ReadPhase::Ways) {
			std::cout << "\nSorting ways" << std::endl;
			osmStore.ways_sort(threadNum);
		}
	}


	osmStore.reportSize();
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

