#ifndef RELATION_ROLES_H
#define RELATION_ROLES_H

#include <boost/container/flat_map.hpp>
#include <mutex>
#include <vector>
#include <string>

class RelationRoles {
public:
	RelationRoles();
	uint16_t getOrAddRole(const std::string& role);
	std::string getRole(uint16_t role) const;

private:
	std::vector<std::string> popularRoleStrings;
	std::vector<std::string> rareRoleStrings;
	std::mutex mutex;
	boost::container::flat_map<std::string, uint16_t> popularRoles;
	boost::container::flat_map<std::string, uint16_t> rareRoles;
};
#endif
