using namespace std;

class SharedData
{
public:
	uint zoom;
	uint mvtVersion;
	int threadNum;
	bool clippingBoxFromJSON;
	double minLon, minLat, maxLon, maxLat;
	OSMObject osmObject;
	OSMStore *osmStore;
	bool includeID, compress, gzip;
	string compressOpt;
	string projectName, projectVersion, projectDesc;
	uint baseZoom, startZoom, endZoom;
	vector<Geometry> cachedGeometries;					// prepared boost::geometry objects (from shapefiles)
	bool verbose;
	bool sqlite;
	MBTiles mbtiles;
	string outputFile;
	map< uint, vector<OutputObject> > *tileIndexForZoom;

	SharedData(kaguya::State *luaPtr, map< string, RTree> *idxPtr, map<uint,string> *namePtr, OSMStore *osmStore) :
		osmObject(luaPtr, idxPtr, &this->cachedGeometries, namePtr, osmStore)
	{
		this->osmStore = osmStore;
		includeID = false, compress = true, gzip = true;
		verbose = false;
		sqlite=false;
		this->tileIndexForZoom = nullptr;
	}

	// ----	Read all config details from JSON file

	void readConfig(const rapidjson::Document &jsonConfig) {
		baseZoom       = jsonConfig["settings"]["basezoom"].GetUint();
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
			minLon = jsonConfig["settings"]["bounding_box"][0].GetDouble();
			minLat = jsonConfig["settings"]["bounding_box"][1].GetDouble();
			maxLon = jsonConfig["settings"]["bounding_box"][2].GetDouble();
			maxLat = jsonConfig["settings"]["bounding_box"][3].GetDouble();
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
	}
};
