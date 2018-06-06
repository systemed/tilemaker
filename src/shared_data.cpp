#include "shared_data.h"
using namespace std;

SharedData::SharedData(const class Config &configIn, const class LayerDefinition &layers,
	class TileData &tileData):
	tileData(tileData),
	layers(layers),
	config(configIn)
{
	sqlite=false;
	verbose = false;
}

SharedData::~SharedData()
{

}

// *****************************************************************

// Define a layer (as read from the .json file)
uint LayerDefinition::addLayer(string name, uint minzoom, uint maxzoom,
		uint simplifyBelow, double simplifyLevel, double simplifyLength, double simplifyRatio, 
		const std::string &source,
		const std::vector<std::string> &sourceColumns,
		bool indexed,
		const std::string &indexName,		
		const std::string &writeTo) 
{
	LayerDef layer = { name, minzoom, maxzoom, simplifyBelow, simplifyLevel, simplifyLength, simplifyRatio, 
		source, sourceColumns, indexed, indexName,
		std::map<std::string,uint>() };
	layers.push_back(layer);
	uint layerNum = layers.size()-1;
	layerMap[name] = layerNum;

	if (writeTo.empty()) {
		vector<uint> r = { layerNum };
		layerOrder.push_back(r);
	} else {
		if (layerMap.count(writeTo) == 0) {
			throw out_of_range("ERROR: addLayer(): the layer to write, named as \"" + writeTo + "\", doesn't exist.");
		}
		uint lookingFor = layerMap[writeTo];
		for (auto it = layerOrder.begin(); it!= layerOrder.end(); ++it) {
			if (it->at(0)==lookingFor) {
				it->push_back(layerNum);
			}
		}
	}
	return layerNum;
}

// *****************************************************************

Config::Config()
{
	includeID = false, compress = true, gzip = true;
	clippingBoxFromJSON = false;
	baseZoom = 0;
	combineSimilarObjs = true;
}

Config::~Config()
{

}

// ----	Read all config details from JSON file

void Config::readConfig(rapidjson::Document &jsonConfig, bool &hasClippingBox, Box &clippingBox) 
{
	baseZoom       = jsonConfig["settings"]["basezoom"].GetUint();
	startZoom      = jsonConfig["settings"]["minzoom" ].GetUint();
	endZoom        = jsonConfig["settings"]["maxzoom" ].GetUint();
	includeID      = jsonConfig["settings"]["include_ids"].GetBool();
	if (! jsonConfig["settings"]["compress"].IsString()) {
		cerr << "\"compress\" should be any of \"gzip\",\"deflate\",\"none\" in JSON file." << endl;
		exit (EXIT_FAILURE);
	}
	compressOpt    = jsonConfig["settings"]["compress"].GetString();
	if(jsonConfig["settings"].HasMember("combine"))
		combineSimilarObjs = jsonConfig["settings"]["combine"].GetBool();
	mvtVersion     = jsonConfig["settings"].HasMember("mvt_version") ? jsonConfig["settings"]["mvt_version"].GetUint() : 2;
	projectName    = jsonConfig["settings"]["name"].GetString();
	projectVersion = jsonConfig["settings"]["version"].GetString();
	projectDesc    = jsonConfig["settings"]["description"].GetString();
	if (jsonConfig["settings"].HasMember("bounding_box")) {
		clippingBoxFromJSON = true;
		hasClippingBox = true;
		minLon = jsonConfig["settings"]["bounding_box"][0].GetDouble();
		minLat = jsonConfig["settings"]["bounding_box"][1].GetDouble();
		maxLon = jsonConfig["settings"]["bounding_box"][2].GetDouble();
		maxLat = jsonConfig["settings"]["bounding_box"][3].GetDouble();
		clippingBox = Box(geom::make<Point>(minLon, lat2latp(minLat)),
		                  geom::make<Point>(maxLon, lat2latp(maxLat)));
	}
	if (jsonConfig["settings"].HasMember("default_view")) {
		defaultView = to_string(jsonConfig["settings"]["default_view"][0].GetDouble()) + "," +
		              to_string(jsonConfig["settings"]["default_view"][1].GetDouble()) + "," +
		              to_string(jsonConfig["settings"]["default_view"][2].GetInt());
	}

	// Check config is valid
	if (endZoom > baseZoom) { cerr << "maxzoom must be the same or smaller than basezoom." << endl; exit (EXIT_FAILURE); }
	if (! compressOpt.empty()) {
		if      (compressOpt == "gzip"   ) { gzip = true;  }
		else if (compressOpt == "deflate") { gzip = false; }
		else if (compressOpt == "none"   ) { compress = false; }
		else {
			cerr << "\"compress\" should be any of \"gzip\",\"deflate\",\"none\" in JSON file." << endl;
			exit (EXIT_FAILURE);
		}
	}

	// Layers
	rapidjson::Value& layerHash = jsonConfig["layers"];
	for (rapidjson::Value::MemberIterator it = layerHash.MemberBegin(); it != layerHash.MemberEnd(); ++it) {

		// Basic layer settings
		string layerName = it->name.GetString();
		int minZoom = it->value["minzoom"].GetInt();
		int maxZoom = it->value["maxzoom"].GetInt();
		string writeTo = it->value.HasMember("write_to") ? it->value["write_to"].GetString() : "";
		int    simplifyBelow  = it->value.HasMember("simplify_below" ) ? it->value["simplify_below" ].GetInt()    : 0;
		double simplifyLevel  = it->value.HasMember("simplify_level" ) ? it->value["simplify_level" ].GetDouble() : 0.01;
		double simplifyLength = it->value.HasMember("simplify_length") ? it->value["simplify_length"].GetDouble() : 0.0;
		double simplifyRatio  = it->value.HasMember("simplify_ratio" ) ? it->value["simplify_ratio" ].GetDouble() : 1.0;
		string source = it->value.HasMember("source") ? it->value["source"].GetString() : "";
		vector<string> sourceColumns;
		if (it->value.HasMember("source_columns")) {
			for (uint i=0; i<it->value["source_columns"].Size(); i++) {
				sourceColumns.push_back(it->value["source_columns"][i].GetString());
			}
		}
		bool indexed=false; if (it->value.HasMember("index")) {
			indexed=it->value["index"].GetBool();
		}
		string indexName = it->value.HasMember("index_column") ? it->value["index_column"].GetString() : "";

		layers.addLayer(layerName, minZoom, maxZoom,
				simplifyBelow, simplifyLevel, simplifyLength, simplifyRatio, 
				source, sourceColumns, indexed, indexName,
				writeTo);

		cout << "Layer " << layerName << " (z" << minZoom << "-" << maxZoom << ")";
		if (it->value.HasMember("write_to")) { cout << " -> " << it->value["write_to"].GetString(); }
		cout << endl;
	}
}

