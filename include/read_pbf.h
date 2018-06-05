#ifndef _READ_PBF_H
#define _READ_PBF_H

#include <string>
#include <unordered_set>
#include <vector>
#include <map>
#include "shared_data.h"

class PbfReader
{
private:
	bool ReadNodes(PrimitiveGroup &pg, const std::unordered_set<int> &nodeKeyPositions, 
		std::map< TileCoordinates, std::vector<OutputObject>, TileCoordinatesCompare > &tileIndex, 
		class OSMStore *osmStore, class OSMObject &osmObject);

	bool ReadWays(PrimitiveGroup &pg, std::unordered_set<WayID> &waysInRelation, 
		std::map< TileCoordinates, std::vector<OutputObject>, TileCoordinatesCompare > &tileIndex, 
		class OSMStore *osmStore, class OSMObject &osmObject);

	bool ReadRelations(PrimitiveGroup &pg, 
		std::map< TileCoordinates, std::vector<OutputObject>, TileCoordinatesCompare > &tileIndex,
		class OSMStore *osmStore, class OSMObject &osmObject);

	void readStringTable(PrimitiveBlock *pbPtr);

	// Find a string in the dictionary
	int findStringPosition(std::string str);

	// Common tag storage
	std::vector<std::string> stringTable;				// Tag table from the current PrimitiveGroup
	std::map<std::string, uint> tagMap;				// String->position map

public:
	PbfReader();
	virtual ~PbfReader();

	int ReadPbfFile(const std::string &inputFile, std::unordered_set<std::string> &nodeKeys, 
		std::map< TileCoordinates, std::vector<OutputObject>, TileCoordinatesCompare > &tileIndex, 
		OSMObject &osmObject);
};

int ReadPbfBoundingBox(const std::string &inputFile, double &minLon, double &maxLon, 
	double &minLat, double &maxLat, bool &hasClippingBox);

#endif //_READ_PBF_H
