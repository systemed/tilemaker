#include <iostream>
#include "read_pbf.h"
#include "pbf_blocks.h"
using namespace std;

bool ReadNodes(PrimitiveGroup &pg, const unordered_set<int> &nodeKeyPositions, 
	std::map< TileCoordinates, std::vector<OutputObject>, TileCoordinatesCompare > &tileIndex, 
	class OSMStore *osmStore, OSMObject &osmObject)
{
	// ----	Read nodes
	NodeStore &nodes = osmStore->nodes;

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
			nodes.insert_back(nodeId, node);
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
				osmObject.setNode(nodeId, &dense, kvStart, kvPos-1, node);
				osmObject.luaState["node_function"](&osmObject);
				if (!osmObject.empty()) {
					TileCoordinates index = latpLon2index(node, osmObject.config.baseZoom);
					for (auto jt = osmObject.outputs.begin(); jt != osmObject.outputs.end(); ++jt) {
						tileIndex[index].push_back(*jt);
					}
				}
			}
		}
		return true;
	}
	return false;
}

bool ReadWays(PrimitiveGroup &pg, unordered_set<WayID> &waysInRelation, 
	std::map< TileCoordinates, std::vector<OutputObject>, TileCoordinatesCompare > &tileIndex, 
	class OSMStore *osmStore, OSMObject &osmObject)
{
	// ----	Read ways
	WayStore &ways = osmStore->ways;

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

			bool ok = true;
			try
			{
				osmObject.setWay(&pbfWay, &nodeVec);
			}
			catch (std::out_of_range &err)
			{
				// Way is missing a node?
				ok = false;
				cerr << endl << err.what() << endl;
			}

			if (ok)
			{
				osmObject.luaState.setErrorHandler(kaguya::ErrorHandler::throwDefaultError);
				kaguya::LuaFunction way_function = osmObject.luaState["way_function"];
				kaguya::LuaRef ret = way_function(&osmObject);
				assert(!ret);
			}

			if (!osmObject.empty() || waysInRelation.count(wayId)) {
				// Store the way's nodes in the global way store
				ways.insert_back(wayId, nodeVec);
			}

			if (!osmObject.empty()) {
				// create a list of tiles this way passes through (tileSet)
				unordered_set<TileCoordinates> tileSet;
				try {
					insertIntermediateTiles(osmStore->nodeListLinestring(nodeVec), osmObject.config.baseZoom, tileSet);

					// then, for each tile, store the OutputObject for each layer
					bool polygonExists = false;
					for (auto it = tileSet.begin(); it != tileSet.end(); ++it) {
						TileCoordinates index = *it;
						for (auto jt = osmObject.outputs.begin(); jt != osmObject.outputs.end(); ++jt) {
							if (jt->geomType == POLYGON) {
								polygonExists = true;
								continue;
							}
							tileIndex[index].push_back(*jt);
						}
					}

					// for polygon, fill inner tiles
					if (polygonExists) {
						fillCoveredTiles(tileSet);
						for (auto it = tileSet.begin(); it != tileSet.end(); ++it) {
							TileCoordinates index = *it;
							for (auto jt = osmObject.outputs.begin(); jt != osmObject.outputs.end(); ++jt) {
								if (jt->geomType != POLYGON) continue;
								tileIndex[index].push_back(*jt);
							}
						}
					}
				} catch(std::out_of_range &err)
				{
					cerr << "Error calculating intermediate tiles: " << err.what() << endl;
				}
			}
		}
		return true;
	}
	return false;
}

bool ReadRelations(PrimitiveGroup &pg, 
	std::map< TileCoordinates, std::vector<OutputObject>, TileCoordinatesCompare > &tileIndex,
	class OSMStore *osmStore, OSMObject &osmObject)
{
	// ----	Read relations
	//		(just multipolygons for now; we should do routes in time)
	RelationStore &relations = osmStore->relations;

	if (pg.relations_size() > 0) {
		int typeKey = osmObject.findStringPosition("type");
		int mpKey   = osmObject.findStringPosition("multipolygon");
		int innerKey= osmObject.findStringPosition("inner");
		//int outerKey= osmObject.findStringPosition("outer");
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

				bool ok = true;
				try
				{
					osmObject.setRelation(&pbfRelation, &outerWayVec, &innerWayVec);
				}
				catch (std::out_of_range &err)
				{
					// Relation is missing a member?
					ok = false;
					cerr << endl << err.what() << endl;
				}

				if (ok)
					osmObject.luaState["way_function"](&osmObject);

				if (!osmObject.empty()) {								

					WayID relID = osmObject.osmID;
					// Store the relation members in the global relation store
					relations.insert_front(relID, outerWayVec, innerWayVec);

					MultiPolygon mp;
					try
					{
						// for each tile the relation may cover, put the output objects.
						mp = osmStore->wayListMultiPolygon(outerWayVec, innerWayVec);
					}
					catch(std::out_of_range &err)
					{
						cout << "In relation " << relID << ": " << err.what() << endl;
						continue;
					}		

					unordered_set<TileCoordinates> tileSet;
					if (mp.size() == 1) {
						insertIntermediateTiles(mp[0].outer(), osmObject.config.baseZoom, tileSet);
						fillCoveredTiles(tileSet);
					} else {
						for (Polygon poly: mp) {
							unordered_set<TileCoordinates> tileSetTmp;
							insertIntermediateTiles(poly.outer(), osmObject.config.baseZoom, tileSetTmp);
							fillCoveredTiles(tileSetTmp);
							tileSet.insert(tileSetTmp.begin(), tileSetTmp.end());
						}
					}

					for (auto it = tileSet.begin(); it != tileSet.end(); ++it) {
						TileCoordinates index = *it;
						for (auto jt = osmObject.outputs.begin(); jt != osmObject.outputs.end(); ++jt) {
							tileIndex[index].push_back(*jt);
						}
					}
				}
			}
		}
		return true;
	}
	return false;
}

int ReadPbfFile(const string &inputFile, unordered_set<string> &nodeKeys, 
	std::map< TileCoordinates, std::vector<OutputObject>, TileCoordinatesCompare > &tileIndex, 
	OSMObject &osmObject)
{
	// ----	Read PBF
	// note that the order of reading and processing is:
	//  1) output nodes -> (remember current position for rewinding to ways) (skip ways) -> (just remember all ways in any relation),
	//  2) output ways, and also construct nodeId list for each way in relation -> output relations

	fstream infile(inputFile, ios::in | ios::binary);
	if (!infile) { cerr << "Couldn't open .pbf file " << inputFile << endl; return -1; }
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
		osmObject.readStringTable(&pb);
		unordered_set<int> nodeKeyPositions;
		for (auto it : nodeKeys) {
			nodeKeyPositions.insert(osmObject.findStringPosition(it));
		}

		for (int i=0; i<pb.primitivegroup_size(); i++) {
			pg = pb.primitivegroup(i);
			cout << "Block " << ct << " group " << i << " ways " << pg.ways_size() << " relations " << pg.relations_size() << "        \r";
			cout.flush();

			bool done = ReadNodes(pg, nodeKeyPositions, tileIndex, osmObject.osmStore, osmObject);
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

			done = ReadWays(pg, waysInRelation, tileIndex, osmObject.osmStore, osmObject);
			if(done) continue;

			done = ReadRelations(pg, tileIndex, osmObject.osmStore, osmObject);
			if(done) continue;

			// Everything should be ended
			break;
		}
		ct++;
	}
	cout << endl;
	infile.close();

	return 0;
}

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

