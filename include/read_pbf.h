#ifndef _READ_PBF_H
#define _READ_PBF_H

#include <string>
#include <unordered_set>
#include <vector>
#include <map>
#include "shared_data.h"

int ReadPbfFile(const std::string &inputFile, std::unordered_set<std::string> &nodeKeys, 
	std::map< uint, std::vector<OutputObject> > &tileIndex, OSMObject &osmObject);

int ReadPbfBoundingBox(const std::string &inputFile, double &minLon, double &maxLon, 
	double &minLat, double &maxLat, bool &hasClippingBox);

#endif //_READ_PBF_H
