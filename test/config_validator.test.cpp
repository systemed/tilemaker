#include <cstdio>
#include <string>
#include <vector>

#include "config_validator.h"
#include "external/minunit.h"
#include "rapidjson/filereadstream.h"

bool validateString(const char* json, std::string &error) {
	rapidjson::Document doc;
	doc.Parse(json);
	if (doc.HasParseError()) {
		error = "parse error";
		return false;
	}
	return validateConfigJson(doc, error);
}

bool validateFile(const char* filename, std::string &error) {
	FILE* fp = fopen(filename, "r");
	if (!fp) {
		error = "could not open file";
		return false;
	}

	char readBuffer[65536];
	rapidjson::FileReadStream is(fp, readBuffer, sizeof(readBuffer));
	rapidjson::Document doc;
	doc.ParseStream(is);
	fclose(fp);
	if (doc.HasParseError()) {
		error = "parse error";
		return false;
	}

	return validateConfigJson(doc, error);
}

MU_TEST(test_valid_minimal_config) {
	std::string error;
	mu_check(validateString(R"({
		"settings": {
			"basezoom": 14,
			"minzoom": 0,
			"maxzoom": 14,
			"include_ids": false,
			"compress": "gzip",
			"name": "Test",
			"version": "1",
			"description": "Test config"
		},
		"layers": {
			"water": {
				"minzoom": 0,
				"maxzoom": 14
			}
		}
	})", error));
	mu_check(error.empty());
}

MU_TEST(test_missing_required_setting) {
	std::string error;
	mu_check(!validateString(R"({
		"settings": {
			"basezoom": 14,
			"minzoom": 0,
			"maxzoom": 14,
			"compress": "gzip",
			"name": "Test",
			"version": "1",
			"description": "Test config"
		},
		"layers": {
			"water": {
				"minzoom": 0,
				"maxzoom": 14
			}
		}
	})", error));
	mu_check(error.find("required") != std::string::npos);
	mu_check(error.find("\"include_ids\"") != std::string::npos);
}

MU_TEST(test_missing_multiple_required_settings) {
	std::string error;
	mu_check(!validateString(R"({
		"settings": {
			"basezoom": 14,
			"minzoom": 0,
			"maxzoom": 14,
			"include_ids": true
		},
		"layers": {
			"transportation": {
				"minzoom": 12,
				"maxzoom": 14
			}
		}
	})", error));
	mu_check(error.find("\"compress\"") != std::string::npos);
	mu_check(error.find("\"name\"") != std::string::npos);
	mu_check(error.find("\"version\"") != std::string::npos);
	mu_check(error.find("\"description\"") != std::string::npos);
}

MU_TEST(test_invalid_layer_type) {
	std::string error;
	mu_check(!validateString(R"({
		"settings": {
			"basezoom": 14,
			"minzoom": 0,
			"maxzoom": 14,
			"include_ids": false,
			"compress": "gzip",
			"name": "Test",
			"version": "1",
			"description": "Test config"
		},
		"layers": {
			"water": {
				"minzoom": "0",
				"maxzoom": 14
			}
		}
	})", error));
	mu_check(error.find("type") != std::string::npos);
	mu_check(error.find("#/layers/water/minzoom") != std::string::npos);
	mu_check(error.find("expected \"integer\", got string") != std::string::npos);
}

MU_TEST(test_invalid_source_columns) {
	std::string error;
	mu_check(!validateString(R"({
		"settings": {
			"basezoom": 14,
			"minzoom": 0,
			"maxzoom": 14,
			"include_ids": false,
			"compress": "gzip",
			"name": "Test",
			"version": "1",
			"description": "Test config"
		},
		"layers": {
			"water": {
				"minzoom": 0,
				"maxzoom": 14,
				"source_columns": false
			}
		}
	})", error));
	mu_check(error.find("oneOf") != std::string::npos);
}

MU_TEST(test_bundled_configs) {
	std::vector<std::string> configs = {
		"resources/config-coastline.json",
		"resources/config-debug.json",
		"resources/config-example.json",
		"resources/config-openmaptiles.json"
	};
	for (const auto &config: configs) {
		std::string error;
		mu_check(validateFile(config.c_str(), error));
		mu_check(error.empty());
	}
}

MU_TEST_SUITE(test_suite_config_validator) {
	MU_RUN_TEST(test_valid_minimal_config);
	MU_RUN_TEST(test_missing_required_setting);
	MU_RUN_TEST(test_missing_multiple_required_settings);
	MU_RUN_TEST(test_invalid_layer_type);
	MU_RUN_TEST(test_invalid_source_columns);
	MU_RUN_TEST(test_bundled_configs);
}

int main() {
	MU_RUN_SUITE(test_suite_config_validator);
	MU_REPORT();
	return MU_EXIT_CODE;
}
