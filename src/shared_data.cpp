#include "shared_data.h"
#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/filereadstream.h"
#include "rapidjson/filewritestream.h"

using namespace std;
using namespace rapidjson;

SharedData::SharedData(Config &configIn, const class LayerDefinition &layers)
	: layers(layers), config(configIn) {
	outputMode=OptionsParser::OutputMode::File;
	mergeSqlite=false;
}

SharedData::~SharedData() { }

// Write project data to .mbtiles file
void SharedData::writeMBTilesProjectData() {
	mbtiles.writeMetadata("name", config.projectName);
	mbtiles.writeMetadata("type", "baselayer");
	mbtiles.writeMetadata("version", config.projectVersion);
	mbtiles.writeMetadata("description", config.projectDesc);
	mbtiles.writeMetadata("format", "pbf");
	mbtiles.writeMetadata("minzoom", to_string(config.startZoom));
	mbtiles.writeMetadata("maxzoom", to_string(config.endZoom));

	ostringstream bounds;
	if (mergeSqlite) {
		double cMinLon, cMaxLon, cMinLat, cMaxLat;
		mbtiles.readBoundingBox(cMinLon, cMaxLon, cMinLat, cMaxLat);
		config.enlargeBbox(cMinLon, cMaxLon, cMinLat, cMaxLat);
	}
	bounds << fixed << config.minLon << "," << config.minLat << "," << config.maxLon << "," << config.maxLat;
	mbtiles.writeMetadata("bounds", bounds.str());

	if (!config.defaultView.empty()) {
		mbtiles.writeMetadata("center", config.defaultView);
	} else {
		double centerLon = (config.minLon + config.maxLon) / 2;
		double centerLat = (config.minLat + config.maxLat) / 2;
		int centerZoom = floor((config.startZoom + config.endZoom) / 2);
		ostringstream center;
		center << fixed << centerLon << "," << centerLat << "," << centerZoom;
		mbtiles.writeMetadata("center",center.str());
	}
}

void SharedData::writeMBTilesMetadata(rapidjson::Document const &jsonConfig) {
	// Write mbtiles 1.3+ json object
	mbtiles.writeMetadata("json", layers.serialiseToJSON());

	// Write user-defined metadata
	if (jsonConfig["settings"].HasMember("metadata")) {
		const rapidjson::Value &md = jsonConfig["settings"]["metadata"];
		for(rapidjson::Value::ConstMemberIterator it=md.MemberBegin(); it != md.MemberEnd(); ++it) {
			if (it->value.IsString()) {
				mbtiles.writeMetadata(it->name.GetString(), it->value.GetString());
			} else {
				rapidjson::StringBuffer strbuf;
				rapidjson::Writer<rapidjson::StringBuffer> writer(strbuf);
				it->value.Accept(writer);
				mbtiles.writeMetadata(it->name.GetString(), strbuf.GetString());
			}
		}
	}
}

void SharedData::writeFileMetadata(rapidjson::Document const &jsonConfig) {
	if(config.compress) 
		std::cout << "When serving compressed tiles, make sure to include 'Content-Encoding: gzip' in your webserver configuration for serving pbf files"  << std::endl;

	rapidjson::Document document;
	document.SetObject();

	if (jsonConfig["settings"].HasMember("filemetadata")) {
		const rapidjson::Value &md = jsonConfig["settings"]["filemetadata"];
		document.CopyFrom(md, document.GetAllocator());
	}

	rapidjson::Value boundsArray(rapidjson::kArrayType);
	boundsArray.PushBack(rapidjson::Value(config.minLon), document.GetAllocator());
	boundsArray.PushBack(rapidjson::Value(config.minLat), document.GetAllocator());
	boundsArray.PushBack(rapidjson::Value(config.maxLon), document.GetAllocator());
	boundsArray.PushBack(rapidjson::Value(config.maxLat), document.GetAllocator());
	document.AddMember("bounds", boundsArray, document.GetAllocator());

	document.AddMember("name",          rapidjson::Value().SetString(config.projectName.c_str(), document.GetAllocator()), document.GetAllocator());
	document.AddMember("version",       rapidjson::Value().SetString(config.projectVersion.c_str(), document.GetAllocator()), document.GetAllocator());
	document.AddMember("description",   rapidjson::Value().SetString(config.projectDesc.c_str(), document.GetAllocator()), document.GetAllocator());
	document.AddMember("minzoom",       rapidjson::Value(config.startZoom), document.GetAllocator());
	document.AddMember("maxzoom",       rapidjson::Value(config.endZoom), document.GetAllocator());
	document.AddMember("vector_layers", layers.serialiseToJSONValue(document.GetAllocator()), document.GetAllocator());

	auto fp = std::fopen((outputFile + "/metadata.json").c_str(), "w");

	char writeBuffer[65536];
	rapidjson::FileWriteStream os(fp, writeBuffer, sizeof(writeBuffer));
	rapidjson::Writer<rapidjson::FileWriteStream> writer(os);
	document.Accept(writer);

	fclose(fp);
}

// Create JSON string with .pmtiles-format metadata
std::string SharedData::pmTilesMetadata(rapidjson::Document const &jsonConfig) {
	rapidjson::Document document;
	document.SetObject();
	document.AddMember("name",          rapidjson::Value().SetString(config.projectName.c_str(), document.GetAllocator()), document.GetAllocator());
	document.AddMember("description",   rapidjson::Value().SetString(config.projectDesc.c_str(), document.GetAllocator()), document.GetAllocator());
	document.AddMember("vector_layers", layers.serialiseToJSONValue(document.GetAllocator()), document.GetAllocator());
	if (jsonConfig["settings"].HasMember("metadata") && jsonConfig["settings"]["metadata"].HasMember("attribution")) {
		document.AddMember("attribution", rapidjson::Value().SetString(jsonConfig["settings"]["metadata"]["attribution"].GetString(), document.GetAllocator()), document.GetAllocator());
	}
	// we don't currently write "type" field, see .pmtiles spec
	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	document.Accept(writer);
	std::string json(buffer.GetString(), buffer.GetSize());
	return json;
}

void SharedData::writePMTilesBounds() {
	pmtiles.header.min_zoom = config.startZoom;
	pmtiles.header.max_zoom = config.endZoom;
	pmtiles.header.center_zoom = (config.startZoom + config.endZoom) / 2;
	pmtiles.header.min_lon_e7 = config.minLon * 10000000;
	pmtiles.header.min_lat_e7 = config.minLat * 10000000;
	pmtiles.header.max_lon_e7 = config.maxLon * 10000000;
	pmtiles.header.max_lat_e7 = config.maxLat * 10000000;
	pmtiles.header.center_lon_e7 = (pmtiles.header.min_lon_e7 + pmtiles.header.max_lon_e7) / 2;
	pmtiles.header.center_lat_e7 = (pmtiles.header.min_lat_e7 + pmtiles.header.max_lat_e7) / 2;
}


// *****************************************************************

// Define a layer (as read from the .json file)
uint LayerDefinition::addLayer(string name, uint minzoom, uint maxzoom,
		uint simplifyBelow, double simplifyLevel, double simplifyLength, double simplifyRatio, uint simplifyAlgo,
		uint filterBelow, double filterArea, bool sortZOrderAscending,
		uint featureLimit, uint featureLimitBelow, bool combinePoints, uint combineLinesBelow, uint combinePolygonsBelow,
		const std::string &source,
		const std::vector<std::string> &sourceColumns,
		bool allSourceColumns,
		bool indexed,
		const std::string &indexName,
		const std::string &writeTo)  {

	bool isWriteTo = !writeTo.empty();
	LayerDef layer = { name, minzoom, maxzoom, simplifyBelow, simplifyLevel, simplifyLength, simplifyRatio, simplifyAlgo,
		filterBelow, filterArea, sortZOrderAscending, featureLimit, featureLimitBelow, combinePoints, combineLinesBelow, combinePolygonsBelow,
		source, sourceColumns, allSourceColumns, indexed, indexName,
		std::map<std::string,uint>(), isWriteTo };
	layers.push_back(layer);
	uint layerNum = layers.size()-1;
	layerMap[name] = layerNum;

	if (writeTo.empty()) {
		vector<uint> r = { layerNum };
		layerOrder.push_back(r);
	} else {
		if (layerMap.count(writeTo) == 0) {
			cerr << "ERROR: addLayer(): the layer to write, named as \"" + writeTo + "\", doesn't exist." << endl;
			exit (EXIT_FAILURE);
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

std::vector<bool> LayerDefinition::getSortOrders() {
	std::vector<bool> orders;
	for (auto &layer : layers) { orders.emplace_back(layer.sortZOrderAscending); }
	return orders;
}

Value LayerDefinition::serialiseToJSONValue(rapidjson::Document::AllocatorType &allocator) const {
	Value layerArray(kArrayType);
	for (auto it = layers.begin(); it != layers.end(); ++it) {
		if (it->writeTo) {
			continue;
		}
		Value fieldObj(kObjectType);
		for (auto jt = it->attributeMap.begin(); jt != it->attributeMap.end(); ++jt) {
			Value k(jt->first.c_str(), allocator);
			switch (jt->second) {
				case 0: fieldObj.AddMember(k, "String" , allocator); break;
				case 1:	fieldObj.AddMember(k, "Number" , allocator); break;
				case 2:	fieldObj.AddMember(k, "Boolean", allocator); break;
			}
		}
		Value layerObj(kObjectType);
		Value name(it->name.c_str(), allocator);
		layerObj.AddMember("id",      name,        allocator);
		layerObj.AddMember("fields",  fieldObj,    allocator);
		layerObj.AddMember("minzoom", it->minzoom, allocator);
		layerObj.AddMember("maxzoom", it->maxzoom, allocator);
		layerArray.PushBack(layerObj, allocator);
	}

	return layerArray;
}

std::string LayerDefinition::serialiseToJSON() const {
	Document document;
	document.SetObject();
	Document::AllocatorType& allocator = document.GetAllocator();

	document.AddMember("vector_layers", serialiseToJSONValue(allocator), allocator);

	StringBuffer buffer;
	Writer<StringBuffer> writer(buffer);
	document.Accept(writer);
	string json(buffer.GetString(), buffer.GetSize());
	return json;
}


// *****************************************************************

Config::Config() {
	includeID = false, compress = true, gzip = true, highResolution = false;
	clippingBoxFromJSON = false;
	baseZoom = 0;
	combineBelow = 0;
}

Config::~Config() { }

// ----	Enlarge existing bounding box

void Config::enlargeBbox(double cMinLon, double cMaxLon, double cMinLat, double cMaxLat) {
	minLon = std::min(minLon, cMinLon);
	maxLon = std::max(maxLon, cMaxLon);
	minLat = std::min(minLat, cMinLat);
	maxLat = std::max(maxLat, cMaxLat);
}

// ----	Read all config details from JSON file

void Config::readConfig(rapidjson::Document &jsonConfig, bool &hasClippingBox, Box &clippingBox)  {
	baseZoom       = jsonConfig["settings"]["basezoom"].GetUint();
	startZoom      = jsonConfig["settings"]["minzoom" ].GetUint();
	endZoom        = jsonConfig["settings"]["maxzoom" ].GetUint();
	includeID      = jsonConfig["settings"]["include_ids"].GetBool();
	highResolution = jsonConfig["settings"].HasMember("high_resolution") && jsonConfig["settings"]["high_resolution"].GetBool();
	if (! jsonConfig["settings"]["compress"].IsString()) {
		cerr << "\"compress\" should be any of \"gzip\",\"deflate\",\"none\" in JSON file." << endl;
		exit (EXIT_FAILURE);
	}
	if (endZoom>15) {
		cout << "**** WARNING ****" << endl;
		cout << "You're generating tiles up to z" << endZoom << ". You probably don't want to do that." << endl;
		cout << "Standard practice is to generate vector tiles up to z14. Your renderer will 'overzoom' the z14 tiles for higher resolutions." << endl;
		cout << "tilemaker may have excessive memory, time, and space requirements at higher zooms. You can find more information in the docs/ folder." << endl;
		cout << "**** WARNING ****" << endl;
	}

	compressOpt    = jsonConfig["settings"]["compress"].GetString();
	combineBelow   = jsonConfig["settings"].HasMember("combine_below") ? jsonConfig["settings"]["combine_below"].GetUint() : 0;
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
	} else if (hasClippingBox) {
		minLon = clippingBox.min_corner().x();
		maxLon = clippingBox.max_corner().x();
		minLat = latp2lat(clippingBox.min_corner().y());
		maxLat = latp2lat(clippingBox.max_corner().y());
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
		double simplifyRatio  = it->value.HasMember("simplify_ratio" ) ? it->value["simplify_ratio" ].GetDouble() : 2.0;
		int    filterBelow    = it->value.HasMember("filter_below"   ) ? it->value["filter_below"   ].GetInt()    : 0;
		double filterArea     = it->value.HasMember("filter_area"    ) ? it->value["filter_area"    ].GetDouble() : 0.5;
		int    featureLimit   = it->value.HasMember("feature_limit"  ) ? it->value["feature_limit"  ].GetInt()    : 0;
		int  featureLimitBelow= it->value.HasMember("feature_limit_below") ? it->value["feature_limit_below"].GetInt() : (maxZoom+1);
		bool combinePoints    = it->value.HasMember("combine_points" ) ? it->value["combine_points" ].GetBool()   : true;
		int combineLinesBelow = it->value.HasMember("combine_lines_below"  ) ? it->value["combine_lines_below"  ].GetInt()    : combineBelow;
		int    combinePolyBelow=it->value.HasMember("combine_polygons_below") ? it->value["combine_polygons_below"].GetInt() : 0;
		bool sortZOrderAscending = it->value.HasMember("z_order_ascending") ? it->value["z_order_ascending"].GetBool() : (featureLimit==0);
		string algo           = it->value.HasMember("simplify_algorithm") ? it->value["simplify_algorithm"].GetString() : "";
		uint simplifyAlgo = algo=="visvalingam" ? LayerDef::VISVALINGAM : LayerDef::DOUGLAS_PEUCKER;
		string source = it->value.HasMember("source") ? it->value["source"].GetString() : "";
		vector<string> sourceColumns;
		bool allSourceColumns = false;
		if (it->value.HasMember("source_columns")) {
			if (it->value["source_columns"].IsTrue()) {
				allSourceColumns = true;
			} else {
				for (uint i=0; i<it->value["source_columns"].Size(); i++) {
					sourceColumns.push_back(it->value["source_columns"][i].GetString());
				}
			}
		}
		bool indexed=false; if (it->value.HasMember("index")) {
			indexed=it->value["index"].GetBool();
		}
		string indexName = it->value.HasMember("index_column") ? it->value["index_column"].GetString() : "";

		layers.addLayer(layerName, minZoom, maxZoom,
				simplifyBelow, simplifyLevel, simplifyLength, simplifyRatio, simplifyAlgo,
				filterBelow, filterArea, sortZOrderAscending, featureLimit, featureLimitBelow, combinePoints, combineLinesBelow, combinePolyBelow,
				source, sourceColumns, allSourceColumns, indexed, indexName,
				writeTo);

		cout << "Layer " << layerName << " (z" << minZoom << "-" << maxZoom << ")";
		if (it->value.HasMember("write_to")) { cout << " -> " << it->value["write_to"].GetString(); }
		cout << endl;
	}
}

