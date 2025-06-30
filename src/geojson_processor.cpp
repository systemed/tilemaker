#include "geojson_processor.h"

#include "helpers.h"
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/post.hpp>

#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/filereadstream.h"

extern bool verbose;

namespace geom = boost::geometry;

// Read GeoJSON, and create OutputObjects for all objects within the specified bounding box
void GeoJSONProcessor::read(class LayerDef &layer, uint layerNum) {
	if (ends_with(layer.source, "JSONL") || ends_with(layer.source, "jsonl") || ends_with(layer.source, "jsonseq") || ends_with(layer.source, "JSONSEQ"))
		return readFeatureLines(layer, layerNum);

	readFeatureCollection(layer, layerNum);
}

void GeoJSONProcessor::readFeatureCollection(class LayerDef &layer, uint layerNum) {
	// Read a JSON file containing a single GeoJSON FeatureCollection object.
	rapidjson::Document doc;
	FILE* fp = fopen(layer.source.c_str(), "r");
	char readBuffer[65536];
	rapidjson::FileReadStream is(fp, readBuffer, sizeof(readBuffer));
	doc.ParseStream(is);
	if (doc.HasParseError()) { throw std::runtime_error("Invalid JSON file."); }
	fclose(fp);

	if (strcmp(doc["type"].GetString(), "FeatureCollection") != 0) { 
		throw std::runtime_error("Top-level GeoJSON object must be a FeatureCollection.");
	}

	// Process each feature
	boost::asio::thread_pool pool(threadNum);
	for (auto &feature : doc["features"].GetArray()) { 
		boost::asio::post(pool, [&]() {
			processFeature(std::move(feature.GetObject()), layer, layerNum);
		});
	}
	pool.join();
}

void GeoJSONProcessor::readFeatureLines(class LayerDef &layer, uint layerNum) {
	// Read a JSON file containing multiple GeoJSON items, newline-delimited.
	std::vector<OffsetAndLength> chunks = getNewlineChunks(layer.source, threadNum * 4);

	// Process each feature
	boost::asio::thread_pool pool(threadNum);
	for (auto &chunk : chunks) { 
		boost::asio::post(pool, [&]() {
			FILE* fp = fopen(layer.source.c_str(), "r");
			if (fseek(fp, chunk.offset, SEEK_SET) != 0) throw std::runtime_error("unable to seek to " + std::to_string(chunk.offset) + " in " + layer.source);
			char readBuffer[65536];
			rapidjson::FileReadStream is(fp, readBuffer, sizeof(readBuffer));

			// Skip leading whitespace.
			while(is.Tell() < chunk.length && isspace(is.Peek())) is.Take();

			while(is.Tell() < chunk.length) {
				auto doc = rapidjson::Document();
				doc.ParseStream<rapidjson::kParseStopWhenDoneFlag>(is);
				if (doc.HasParseError()) { throw std::runtime_error("Invalid JSON file."); }
				processFeature(std::move(doc.GetObject()), layer, layerNum);

				// Skip trailing whitespace.
				while(is.Tell() < chunk.length && isspace(is.Peek())) is.Take();
			}
			fclose(fp);
		});
	}
	pool.join();
}

template <bool Flag, typename T>
void GeoJSONProcessor::processFeature(rapidjson::GenericObject<Flag, T> feature, class LayerDef &layer, uint layerNum) {

	// Recurse if it's a FeatureCollection
	std::string type = feature["type"].GetString();
	if (type == "FeatureCollection") { 
		for (auto &f : feature["features"].GetArray()) {
			processFeature(std::move(f.GetObject()), layer, layerNum);
		}
		return;
	}

	// Read properties
	bool hasName = false;
	std::string name;
	const rapidjson::Value &pr = feature["properties"];
	unsigned minzoom = layer.minzoom;
	AttributeIndex attrIdx = readProperties(pr, hasName, name, layer, minzoom);

	// Parse geometry
	auto geometry = feature["geometry"].GetObject();
	std::string geomType = geometry["type"].GetString();
	if (geomType=="GeometryCollection") { 
		// TODO: handle GeometryCollection (just put normal geometries in a list, then we can iterate through whatever we have)
		// (maybe do it with a lambda)
		std::cerr << "GeometryCollection not currently supported." << std::endl;
		return;
	}
	auto coords = geometry["coordinates"].GetArray();

	// Convert each type of GeoJSON geometry into Boost.Geometry equivalent
	if (geomType=="Point") {
		// coordinates is [x,y]
		Point p( coords[0].GetDouble(), lat2latp(coords[1].GetDouble()) );
		if (geom::within(p, clippingBox)) {
			shpMemTiles.StoreGeometry(layerNum, layer.name, POINT_, p, layer.indexed, hasName, name, minzoom, attrIdx);
		}

	} else if (geomType=="LineString") {
		// coordinates is [[x,y],[x,y],[x,y]...]
		Linestring ls;
		geom::assign_points(ls, pointsFromGeoJSONArray(coords));
		MultiLinestring out;
		geom::intersection(ls, clippingBox, out);
		if (!geom::is_empty(out)) {
			shpMemTiles.StoreGeometry(layerNum, layer.name, MULTILINESTRING_, out, layer.indexed, hasName, name, minzoom, attrIdx);
		}

	} else if (geomType=="Polygon") {
		// coordinates is [ Ring, Ring, Ring... ]
		// where Ring is [[x,y],[x,y],[x,y]...]
		Polygon polygon = polygonFromGeoJSONArray(coords);
		geom::correct(polygon);
		MultiPolygon out;
		geom::intersection(polygon, clippingBox, out);
		if (!geom::is_empty(out)) {
			shpMemTiles.StoreGeometry(layerNum, layer.name, POLYGON_, out, layer.indexed, hasName, name, minzoom, attrIdx);
		}

	} else if (geomType=="MultiPoint") {
		// coordinates is [[x,y],[x,y],[x,y]...]
		for (auto &pt : coords) {
			Point p( pt[0].GetDouble(), lat2latp(pt[1].GetDouble()) );
			if (geom::within(p, clippingBox)) {
				shpMemTiles.StoreGeometry(layerNum, layer.name, POINT_, p, layer.indexed, hasName, name, minzoom, attrIdx);
			}
		}
		
	} else if (geomType=="MultiLineString") {
		// coordinates is [ LineString, LineString, LineString... ]
		MultiLinestring mls;
		for (auto &pts : coords) {
			Linestring ls;
			geom::assign_points(ls, pointsFromGeoJSONArray(pts.GetArray()));
			mls.emplace_back(ls);
		}
		MultiLinestring out;
		geom::intersection(mls, clippingBox, out);
		if (!geom::is_empty(out)) {
			shpMemTiles.StoreGeometry(layerNum, layer.name, MULTILINESTRING_, out, layer.indexed, hasName, name, minzoom, attrIdx);
		}

	} else if (geomType=="MultiPolygon") {
		// coordinates is [ Polygon, Polygon, Polygon... ]
		MultiPolygon mp;
		for (auto &p : coords) {
			mp.emplace_back(polygonFromGeoJSONArray(p.GetArray()));
		}
		geom::correct(mp);
		MultiPolygon out;
		geom::intersection(mp, clippingBox, out);
		if (!geom::is_empty(out)) {
			shpMemTiles.StoreGeometry(layerNum, layer.name, POLYGON_, out, layer.indexed, hasName, name, minzoom, attrIdx);
		}
	}
}

template <bool Flag, typename T>
Polygon GeoJSONProcessor::polygonFromGeoJSONArray(const rapidjson::GenericArray<Flag, T> &coords) {
	Polygon poly;
	bool first = true;
	for (auto &r : coords) {
		Ring ring;
		geom::assign_points(ring, pointsFromGeoJSONArray(r.GetArray()));
		if (first) { poly.outer() = std::move(ring); first = false; }
		else { poly.inners().emplace_back(std::move(ring)); }
	}
	return poly;
}

template <bool Flag, typename T>
std::vector<Point> GeoJSONProcessor::pointsFromGeoJSONArray(const rapidjson::GenericArray<Flag, T> &arr) {
	std::vector<Point> points;
	for (auto &pt : arr) {
		points.emplace_back(Point( pt[0].GetDouble(), lat2latp(pt[1].GetDouble()) ));
	}
	return points;
}

// Read properties and generate an AttributeIndex
AttributeIndex GeoJSONProcessor::readProperties(const rapidjson::Value &pr, bool &hasName, std::string &name, LayerDef &layer, unsigned &minzoom) {
	std::lock_guard<std::mutex> lock(attributeMutex);
	AttributeStore& attributeStore = osmLuaProcessing.getAttributeStore();
	AttributeSet attributes;

	// Name for indexing?
	if (layer.indexName.length()>0) {
		auto n = pr.FindMember(layer.indexName.c_str());
		if (n != pr.MemberEnd()) {
			hasName = true;
			name = n->value.GetString();
		}
	}

	if (osmLuaProcessing.canRemapShapefiles()) {
		// Create table object
		kaguya::LuaTable in_table = osmLuaProcessing.newTable();
		for (rapidjson::Value::ConstMemberIterator it = pr.MemberBegin(); it != pr.MemberEnd(); ++it) {
			std::string key = it->name.GetString();
			if (!layer.useColumn(key)) continue;
			if (it->value.IsString()) { in_table[key] = it->value.GetString(); }
			else if (it->value.IsDouble()) { in_table[key] = it->value.GetDouble(); }
			else if (it->value.IsNumber()) { in_table[key] = it->value.GetInt(); }
			else {
				// something different, so coerce to string
				rapidjson::StringBuffer strbuf;
				rapidjson::Writer<rapidjson::StringBuffer> writer(strbuf);
				it->value.Accept(writer);
				in_table[key] = strbuf.GetString();
			}
		}

		// Call remap function
		kaguya::LuaTable out_table = osmLuaProcessing.remapAttributes(in_table, layer.name);

		// Write values to vector tiles
		// (c&p from shp_processor, could be refactored)
		for (auto key : out_table.keys()) {
			kaguya::LuaRef val = out_table[key];
			if (val.isType<std::string>()) {
				attributeStore.addAttribute(attributes, key, static_cast<const std::string&>(val), 0);
				layer.attributeMap[key] = 0;
			} else if (val.isType<int>()) {
				if (key=="_minzoom") { minzoom=val; continue; }
				attributeStore.addAttribute(attributes, key, (int)val, 0);
				layer.attributeMap[key] = 1;
			} else if (val.isType<double>()) {
				attributeStore.addAttribute(attributes, key, (double)val, 0);
				layer.attributeMap[key] = 1;
			} else if (val.isType<bool>()) {
				attributeStore.addAttribute(attributes, key, (bool)val, 0);
				layer.attributeMap[key] = 2;
			} else {
				// don't even think about trying to write nested tables, thank you
				std::cout << "Didn't recognise Lua output type: " << val << std::endl;
			}
		}

	} else {
		
		// No remapping, so just copy across
		for (rapidjson::Value::ConstMemberIterator it = pr.MemberBegin(); it != pr.MemberEnd(); ++it) {
			std::string key = it->name.GetString();
			if (!layer.useColumn(key)) continue;
			if (it->value.IsString()) { 
				attributeStore.addAttribute(attributes, key, it->value.GetString(), 0);
				layer.attributeMap[key] = 0;
			} else if (it->value.IsBool()) { 
				attributeStore.addAttribute(attributes, key, it->value.GetBool(), 0);
				layer.attributeMap[key] = 2;
			} else if (it->value.IsNumber()) { 
				attributeStore.addAttribute(attributes, key, it->value.GetDouble(), 0);
				layer.attributeMap[key] = 1;
			} else {
				// something different, so coerce to string
				rapidjson::StringBuffer strbuf;
				rapidjson::Writer<rapidjson::StringBuffer> writer(strbuf);
				it->value.Accept(writer);
				attributeStore.addAttribute(attributes, key, strbuf.GetString(), 0);
				layer.attributeMap[key] = 0;
			}
		}
	}
	return attributeStore.add(attributes);
}
