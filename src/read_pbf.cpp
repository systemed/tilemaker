#include <iostream>
#include "read_pbf.h"
#include "pbf_blocks.h"

#include <boost/interprocess/streams/bufferstream.hpp>

using namespace std;

PbfReader::PbfReader(OSMStore &osmStore)
	: osmStore(osmStore)
{
	output = nullptr;
}

bool PbfReader::ReadNodes(PrimitiveGroup &pg, PrimitiveBlock const &pb, const unordered_set<int> &nodeKeyPositions)
{
	// ----	Read nodes

	if (pg.has_dense()) {
		int64_t nodeId  = 0;
		int lon = 0;
		int lat = 0;
		int kvPos = 0;
		DenseNodes dense = pg.dense();
		for (int j=0; j<dense.id_size(); j++) {
			nodeId += dense.id(j);
			lon    += dense.lon(j);
			lat    += dense.lat(j);
			LatpLon node = { int(lat2latp(double(lat)/10000000.0)*10000000.0), lon };

			osmStore.nodes_insert_back(nodeId, node);

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
			if (significant) {
				boost::container::flat_map<std::string, std::string> tags;
				for (uint n=kvStart; n<kvPos-1; n+=2) {
					tags[pb.stringtable().s(dense.keys_vals(n))] = pb.stringtable().s(dense.keys_vals(n+1));
				}

				output->setNode(static_cast<NodeID>(nodeId), node, tags);
			}
		}
		return true;
	}
	return false;
}

bool PbfReader::ReadWays(PrimitiveGroup &pg, PrimitiveBlock const &pb) {
	// ----	Read ways

	if (pg.ways_size() > 0) {
		Way pbfWay;
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
				OSMStore::handle_t handle = osmStore.ways_insert_back(static_cast<WayID>(pbfWay.id()), nodeVec);
				output->setWay(static_cast<WayID>(pbfWay.id()), handle, tags);

			} catch (std::out_of_range &err) {
				// Way is missing a node?
				cerr << endl << err.what() << endl;
			}

		}
		return true;
	}
	return false;
}

bool PbfReader::ReadRelations(PrimitiveGroup &pg, PrimitiveBlock const &pb) {
	// ----	Read relations
	//		(just multipolygons for now; we should do routes in time)

	if (pg.relations_size() > 0) {
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
	 				OSMStore::handle_t handle = osmStore.relations_insert_front(pbfRelation.id(), outerWayVec, innerWayVec);
					output->setRelation(pbfRelation.id(), handle, tags);

				} catch (std::out_of_range &err) {
					// Relation is missing a member?
					cerr << endl << err.what() << endl;
				}

			}
		}
		return true;
	}
	return false;
}

int PbfReader::ReadPbfFile(std::istream &infile, unordered_set<string> &nodeKeys)
{
	// ----	Read PBF
	osmStore.clear();

	HeaderBlock block;
	readBlock(&block, infile);

	uint ct=0;

	while (true) {
		PrimitiveBlock pb;
		readBlock(&pb, infile);
		if (infile.eof()) {
			break;
		}

		// Read the string table, and pre-calculate the positions of valid node keys
		unordered_set<int> nodeKeyPositions;
		for (auto it : nodeKeys) {
			nodeKeyPositions.insert(findStringPosition(pb, it.c_str()));
		}

		for (int i=0; i<pb.primitivegroup_size(); i++) {
			PrimitiveGroup pg;
			pg = pb.primitivegroup(i);
			cout << "Block " << ct << " group " << i << " ways " << pg.ways_size() << " relations " << pg.relations_size() << "        \r";
			cout.flush();

			bool done = ReadNodes(pg, pb, nodeKeyPositions);
			if(done) continue;

			done = ReadWays(pg, pb);
			if(done) continue;

			done = ReadRelations(pg, pb);
			if(done) continue;
		}
		ct++;
	}
	cout << endl;

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
	readBlock(&block, infile);
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

void PbfIndexWriter::setNode(NodeID id, LatpLon node, const tag_map_t &tags)
{
	osmStore.pbf_store_node_entry(id, node, tags);
}

void PbfIndexWriter::setWay(WayID wayId, OSMStore::handle_t nodeVecHandle, const tag_map_t &tags) 
{
	osmStore.pbf_store_way_entry(wayId, nodeVecHandle, tags);
}

void PbfIndexWriter::setRelation(int64_t relationId, OSMStore::handle_t relationHandle, const tag_map_t &tags)
{
	osmStore.pbf_store_relation_entry(relationId, relationHandle, tags);
}

void PbfIndexWriter::save(std::string const &filename)
{
	osmStore.save(filename);
}
