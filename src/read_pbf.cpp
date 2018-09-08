#include <iostream>
#include "read_pbf.h"
#include "pbf_blocks.h"
using namespace std;

PbfReader::PbfReader()
{
	output = nullptr;
}

PbfReader::~PbfReader()
{

}

bool PbfReader::ReadNodes(PrimitiveGroup &pg, const unordered_set<int> &nodeKeyPositions)
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
			if(output != nullptr)
				output->everyNode(nodeId, node);
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
				std::map<std::string, std::string> tags;
				for (uint n=kvStart; n<kvPos-1; n+=2)
					tags[stringTable[dense.keys_vals(n)]] = stringTable[dense.keys_vals(n+1)];

				if(output != nullptr)
					output->setNode(nodeId, node, tags);
			}
		}
		return true;
	}
	return false;
}

bool PbfReader::ReadWays(PrimitiveGroup &pg, unordered_set<WayID> &waysInRelation)
{
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

			try
			{
				auto keysPtr = pbfWay.mutable_keys();
				auto valsPtr = pbfWay.mutable_vals();
				std::map<std::string, std::string> tags;
				for (uint n=0; n < pbfWay.keys_size(); n++)
					tags[stringTable[keysPtr->Get(n)]] = stringTable[valsPtr->Get(n)];

				if(output != nullptr)
					output->setWay(&pbfWay, &nodeVec, waysInRelation.count(wayId), tags);
			}
			catch (std::out_of_range &err)
			{
				// Way is missing a node?
				cerr << endl << err.what() << endl;
			}

		}
		return true;
	}
	return false;
}

bool PbfReader::ReadRelations(PrimitiveGroup &pg)
{
	// ----	Read relations
	//		(just multipolygons for now; we should do routes in time)

	if (pg.relations_size() > 0) {
		int typeKey = this->findStringPosition("type");
		int mpKey   = this->findStringPosition("multipolygon");
		int innerKey= this->findStringPosition("inner");
		//int outerKey= this->findStringPosition("outer");
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

				try
				{
					auto keysPtr = pbfRelation.mutable_keys();
					auto valsPtr = pbfRelation.mutable_vals();
					std::map<std::string, std::string> tags;
					for (uint n=0; n < pbfRelation.keys_size(); n++)
						tags[stringTable[keysPtr->Get(n)]] = stringTable[valsPtr->Get(n)];

					if(output != nullptr)
						output->setRelation(&pbfRelation, &outerWayVec, &innerWayVec, tags);
				}
				catch (std::out_of_range &err)
				{
					// Relation is missing a member?
					cerr << endl << err.what() << endl;
				}

			}
		}
		return true;
	}
	return false;
}

int PbfReader::ReadPbfFile(const string &inputFile, unordered_set<string> &nodeKeys)
{
	// ----	Read PBF
	// note that the order of reading and processing is:
	//  1) output nodes -> (remember current position for rewinding to ways) (skip ways) -> (just remember all ways in any relation),
	//  2) output ways, and also construct nodeId list for each way in relation -> output relations

	fstream infile(inputFile, ios::in | ios::binary);
	if (!infile) { cerr << "Couldn't open .pbf file " << inputFile << endl; return -1; }

	if(output != nullptr)
		output->startOsmData();

	HeaderBlock block;
	readBlock(&block, &infile);

	PrimitiveBlock pb;
	PrimitiveGroup pg;
	vector<string> strings(0);
	uint ct=0;
	bool checkedRelations = false;
	long long wayPosition = -1;
	unordered_set<WayID> waysInRelation;

	while (true) {
		long long blockStart = infile.tellg();
		readBlock(&pb, &infile);
		if (infile.eof()) {
			if (!checkedRelations) {
				checkedRelations = true;
			} else {
				break;
			}
		    if (wayPosition==-1) {
				cout << ".pbf does not contain any ways" << endl;
				break;
			}
			infile.clear();
			infile.seekg(wayPosition);
			continue;
		}

		// Read the string table, and pre-calculate the positions of valid node keys
		this->readStringTable(&pb);
		unordered_set<int> nodeKeyPositions;
		for (auto it : nodeKeys) {
			nodeKeyPositions.insert(this->findStringPosition(it));
		}

		for (int i=0; i<pb.primitivegroup_size(); i++) {
			pg = pb.primitivegroup(i);
			cout << "Block " << ct << " group " << i << " ways " << pg.ways_size() << " relations " << pg.relations_size() << "        \r";
			cout.flush();

			bool done = ReadNodes(pg, nodeKeyPositions);
			if(done) continue;

			// ----	Remember the position and skip ways

			if (!checkedRelations && pg.ways_size() > 0) {
				if (wayPosition == -1) {
					wayPosition = blockStart;
				}
				continue;
			}

			// ----	Remember all ways in any relation

			if (!checkedRelations && pg.relations_size() > 0) {
				for (int j=0; j<pg.relations_size(); j++) {
					Relation pbfRelation = pg.relations(j);
					int64_t lastID = 0;
					for (int n = 0; n < pbfRelation.memids_size(); n++) {
						lastID += pbfRelation.memids(n);
						if (pbfRelation.types(n) != Relation_MemberType_WAY) { continue; }
						WayID wayId = static_cast<WayID>(lastID);
						waysInRelation.insert(wayId);
					}
				}
				continue;
			}

			if (!checkedRelations) {
				// Nothing to do
				break;
			}

			done = ReadWays(pg, waysInRelation);
			if(done) continue;

			done = ReadRelations(pg);
			if(done) continue;

			// Everything should be ended
			break;
		}
		ct++;
	}
	cout << endl;
	infile.close();

	if(output != nullptr)
		output->endOsmData();

	return 0;
}

// Read string dictionary from the .pbf
void PbfReader::readStringTable(PrimitiveBlock *pbPtr) {
	// Populate the string table
	stringTable.clear();
	stringTable.resize(pbPtr->stringtable().s_size());
	for (int i=0; i<pbPtr->stringtable().s_size(); i++) {
		stringTable[i] = pbPtr->stringtable().s(i);
	}
	// Create a string->position map
	tagMap.clear();
	for (int i=0; i<pbPtr->stringtable().s_size(); i++) {
		tagMap.insert(pair<string, int> (pbPtr->stringtable().s(i), i));
	}
}

// Find a string in the dictionary
int PbfReader::findStringPosition(string str) {
	auto p = find(stringTable.begin(), stringTable.end(), str);
	if (p == stringTable.end()) {
		return -1;
	} else {
		return distance(stringTable.begin(), p);
	}
}

// *************************************************

int ReadPbfBoundingBox(const std::string &inputFile, double &minLon, double &maxLon, 
	double &minLat, double &maxLat, bool &hasClippingBox)
{
	fstream infile(inputFile, ios::in | ios::binary);
	if (!infile) { cerr << "Couldn't open .pbf file " << inputFile << endl; return -1; }
	HeaderBlock block;
	readBlock(&block, &infile);
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

