#include "shared_data.h"
#include "read_shp.h"
using namespace std;

SharedData::SharedData(OSMStore *osmStore)
{
	this->osmStore = osmStore;
	sqlite=false;
	this->tileIndexForZoom = nullptr;
	verbose = false;
}

SharedData::~SharedData()
{

}

// *****************************************************************

Config::Config()
{
	includeID = false, compress = true, gzip = true;
}

Config::~Config()
{

}

// ----	Read all config details from JSON file

void Config::readConfig(rapidjson::Document &jsonConfig, bool hasClippingBox, Box &clippingBox,
                map< uint, vector<OutputObject> > &tileIndex, OSMObject &osmObject) {
	baseZoom       = jsonConfig["settings"]["basezoom"].GetUint();
	osmObject.baseZoom = baseZoom;
	startZoom      = jsonConfig["settings"]["minzoom" ].GetUint();
	endZoom        = jsonConfig["settings"]["maxzoom" ].GetUint();
	includeID      = jsonConfig["settings"]["include_ids"].GetBool();
	if (! jsonConfig["settings"]["compress"].IsString()) {
		cerr << "\"compress\" should be any of \"gzip\",\"deflate\",\"none\" in JSON file." << endl;
		exit (EXIT_FAILURE);
	}
	compressOpt    = jsonConfig["settings"]["compress"].GetString();
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
		uint layerNum = osmObject.addLayer(layerName, minZoom, maxZoom,
				simplifyBelow, simplifyLevel, simplifyLength, simplifyRatio, writeTo);
		cout << "Layer " << layerName << " (z" << minZoom << "-" << maxZoom << ")";
		if (it->value.HasMember("write_to")) { cout << " -> " << it->value["write_to"].GetString(); }
		cout << endl;

		// External layer sources
		if (it->value.HasMember("source")) {
			if (!hasClippingBox) {
				cerr << "Can't read shapefiles unless a bounding box is provided." << endl;
				exit(EXIT_FAILURE);
			}
			vector<string> sourceColumns;
			if (it->value.HasMember("source_columns")) {
				for (uint i=0; i<it->value["source_columns"].Size(); i++) {
					sourceColumns.push_back(it->value["source_columns"][i].GetString());
				}
			}
			bool indexed=false; if (it->value.HasMember("index")) {
				indexed=it->value["index"].GetBool();
				osmObject.indices->operator[](layerName)=RTree();
			}
			string indexName = it->value.HasMember("index_column") ? it->value["index_column"].GetString() : "";
			readShapefile(it->value["source"].GetString(), sourceColumns, clippingBox, tileIndex,
			              cachedGeometries,
			              osmObject,
			              baseZoom, layerNum, layerName, indexed,
			              indexName);
		}
	}
}

