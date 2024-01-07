#include "relation_roles.h"

RelationRoles::RelationRoles() {
	// Computed in early 2024 from popular roles: https://gist.github.com/systemed/29ea4c8d797a20dcdffee8ba907d62ea
	// This is just an optimization to avoid taking a lock in the common case.
	//
	// The list should be refreshed if the set of popular roles dramatically changes,
	// but tilemaker will still be correct, just slower.
	popularRoleStrings = {
		"",
		"1700","1800","1900","2700","2800","2900","3000","3100","3200","above","accessfrom",
		"accessto","accessvia","across","address","admin_centre","alternative","associated",
		"attached_to","backward","basket","both","branch_circuit","building","buildingpart",
		"builidingpart","camera","claimed","connection","contains","crossing","destination",
		"device","de_facto","east","edge","empty role","end","endpoint","entrance","entry",
		"ex-camera","exit","extent","facility","force","forward","from","generator","give_way",
		"graph","guidepost","hidden","highway","Hole","hole","house","inner","intersection",
		"in_tunnel","junction","label","lable","landuse","lateral","left","line","location_hint",
		"lower","main","main_stream","marker","member","memorial","mirror","negative",
		"negative:entry","negative:exit","negative:parking","object","on_bridge","outer",
		"outline","part","pedestrian","pin","pit_lane","platform","positive","positive:entry",
		"positive:exit","positive:parking","priority","ridge","right","road_marking","road_sign",
		"room","section","sector","shell","side_stream","sign","signal","start","stop","street",
		"sub-relation","subarea","subbasin","substation","switch","target","tee","through","to",
		"tomb","track","tracksection","traffic_sign","tributary","trunk_circuit","under","upper",
		"via","visible","walk","ways","west"
	};

	for (const auto& s : popularRoleStrings) {
		popularRoles[s] = popularRoles.size();
	}
}

std::string RelationRoles::getRole(uint16_t role) const {
	if (role < popularRoleStrings.size())
		return popularRoleStrings[role];

	return rareRoleStrings[role - popularRoleStrings.size()];
}

uint16_t RelationRoles::getOrAddRole(const std::string& role) {
	{
		const auto& pos = popularRoles.find(role);
		if (pos != popularRoles.end())
			return pos->second;
	}

	std::lock_guard<std::mutex> lock(mutex);
	const auto& pos = rareRoles.find(role);
	if (pos != rareRoles.end())
		return pos->second;

	uint16_t rv = popularRoleStrings.size() + rareRoleStrings.size();
	rareRoles[role] = rv;
	rareRoleStrings.push_back(role);
	return rv;
}
