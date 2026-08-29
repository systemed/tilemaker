#ifndef _CONFIG_VALIDATOR_H
#define _CONFIG_VALIDATOR_H

#include <string>

#include "rapidjson/document.h"

bool validateConfigJson(const rapidjson::Document &jsonConfig, std::string &error);

#endif //_CONFIG_VALIDATOR_H
