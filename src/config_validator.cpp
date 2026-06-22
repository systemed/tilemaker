#include "config_validator.h"

#include <config_schema.h>

#include <cstring>
#include <vector>

#include "rapidjson/pointer.h"
#include "rapidjson/schema.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

namespace {
std::string pointerString(const rapidjson::Pointer &pointer) {
	rapidjson::StringBuffer buffer;
	pointer.StringifyUriFragment(buffer);
	return buffer.GetString();
}

std::string valueToString(const rapidjson::Value &value) {
	if (value.IsString()) return std::string("\"") + value.GetString() + "\"";

	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	value.Accept(writer);
	return buffer.GetString();
}

std::string joinValues(const std::vector<std::string> &values) {
	std::string joined;
	for (std::size_t i = 0; i < values.size(); i++) {
		if (i > 0) joined += ", ";
		joined += values[i];
	}
	return joined;
}

std::string valueTypeName(const rapidjson::Value &value) {
	if (value.IsNull()) return "null";
	if (value.IsBool()) return "boolean";
	if (value.IsObject()) return "object";
	if (value.IsArray()) return "array";
	if (value.IsString()) return "string";
	if (value.IsNumber()) return "number";
	return "unknown";
}

std::string requiredError(const rapidjson::Document &schemaJson,
                          const rapidjson::Document &jsonConfig,
                          const rapidjson::Pointer &schemaPointer,
                          const rapidjson::Pointer &documentPointer,
                          const std::string &documentPointerString) {
	const rapidjson::Value* schemaNode = schemaPointer.Get(schemaJson);
	const rapidjson::Value* documentNode = documentPointer.Get(jsonConfig);
	if (!schemaNode || !schemaNode->IsObject() || !schemaNode->HasMember("required") ||
	    !(*schemaNode)["required"].IsArray() || !documentNode || !documentNode->IsObject()) {
		return "";
	}

	std::vector<std::string> missing;
	for (rapidjson::Value::ConstValueIterator it = (*schemaNode)["required"].Begin(); it != (*schemaNode)["required"].End(); ++it) {
		if (it->IsString() && !documentNode->HasMember(it->GetString())) {
			missing.push_back(std::string("\"") + it->GetString() + "\"");
		}
	}
	if (missing.empty()) return "";

	return "missing required " + std::string(missing.size() == 1 ? "field " : "fields ") +
	       joinValues(missing) + " at " + documentPointerString;
}

std::string typeError(const rapidjson::Document &schemaJson,
                      const rapidjson::Document &jsonConfig,
                      const rapidjson::Pointer &schemaPointer,
                      const rapidjson::Pointer &documentPointer,
                      const std::string &documentPointerString) {
	const rapidjson::Value* schemaNode = schemaPointer.Get(schemaJson);
	const rapidjson::Value* documentNode = documentPointer.Get(jsonConfig);
	if (!schemaNode || !schemaNode->IsObject() || !schemaNode->HasMember("type") || !documentNode) {
		return "";
	}

	return "invalid type at " + documentPointerString + ": expected " +
	       valueToString((*schemaNode)["type"]) + ", got " + valueTypeName(*documentNode);
}

std::string enumError(const rapidjson::Document &schemaJson,
                      const rapidjson::Document &jsonConfig,
                      const rapidjson::Pointer &schemaPointer,
                      const rapidjson::Pointer &documentPointer,
                      const std::string &documentPointerString) {
	const rapidjson::Value* schemaNode = schemaPointer.Get(schemaJson);
	const rapidjson::Value* documentNode = documentPointer.Get(jsonConfig);
	if (!schemaNode || !schemaNode->IsObject() || !schemaNode->HasMember("enum") ||
	    !(*schemaNode)["enum"].IsArray() || !documentNode) {
		return "";
	}

	std::vector<std::string> allowed;
	for (rapidjson::Value::ConstValueIterator it = (*schemaNode)["enum"].Begin(); it != (*schemaNode)["enum"].End(); ++it) {
		allowed.push_back(valueToString(*it));
	}

	return "invalid value at " + documentPointerString + ": expected one of " +
	       joinValues(allowed) + ", got " + valueToString(*documentNode);
}
} // namespace

bool validateConfigJson(const rapidjson::Document &jsonConfig, std::string &error) {
	rapidjson::Document schemaJson;
	schemaJson.Parse(CONFIG_SCHEMA);
	if (schemaJson.HasParseError()) {
		error = "Internal config schema is invalid.";
		return false;
	}

	rapidjson::SchemaDocument schema(schemaJson);
	rapidjson::SchemaValidator validator(schema);
	if (jsonConfig.Accept(validator)) {
		return true;
	}

	std::string documentPointer = pointerString(validator.GetInvalidDocumentPointer());
	if (documentPointer.empty()) documentPointer = "#";
	std::string schemaPointer = pointerString(validator.GetInvalidSchemaPointer());
	if (schemaPointer.empty()) schemaPointer = "#";

	const char* keyword = validator.GetInvalidSchemaKeyword();
	if (std::strcmp(keyword, "required") == 0) {
		error = requiredError(schemaJson, jsonConfig, validator.GetInvalidSchemaPointer(),
		                      validator.GetInvalidDocumentPointer(), documentPointer);
	} else if (std::strcmp(keyword, "type") == 0) {
		error = typeError(schemaJson, jsonConfig, validator.GetInvalidSchemaPointer(),
		                  validator.GetInvalidDocumentPointer(), documentPointer);
	} else if (std::strcmp(keyword, "enum") == 0) {
		error = enumError(schemaJson, jsonConfig, validator.GetInvalidSchemaPointer(),
		                  validator.GetInvalidDocumentPointer(), documentPointer);
	}

	if (error.empty()) {
		error = "schema validation failed at " + documentPointer + ": " +
		        keyword + " (" + schemaPointer + ")";
	}
	return false;
}
