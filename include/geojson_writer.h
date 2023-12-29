/*! \file */ 
#ifndef _GEOJSON_WRITER_H
#define _GEOJSON_WRITER_H

/*
	GeoJSON writer for boost::geometry objects, using RapidJSON.
	This isn't core tilemaker functionality but helps with debugging.
	As yet it only outputs (Multi)Polygons but can be extended for more types.

	Example:
		auto gj = GeoJSONWriter()
		gj.addGeometry(myMultiPolygon);
		gj.finalise();
		std::cout << gj.toString() << std::endl;

	Or use gj.toFile("output.geojson") to write to file.
	Calling finalise(true) will 'unproject' Y values (i.e. latp2lat).

	Todo: support more geometries, set/write properties.
*/

#include <iostream>
#include <math.h>
#include "geom.h"

#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/filewritestream.h"

typedef boost::variant<Point,Linestring,MultiLinestring,Polygon,MultiPolygon,Ring> AnyGeometry;

using namespace rapidjson;

struct GeoJSONWriter {
	Document document;
	std::vector<AnyGeometry> geometries;

	GeoJSONWriter() {
		document.SetObject();
		document.AddMember("type", Value().SetString("FeatureCollection"), document.GetAllocator());
	}
	void addGeometry(AnyGeometry geom) {
		geometries.emplace_back(geom);
	}
	struct SerialiseGeometry {
		Value *obj;
		Document::AllocatorType *alloc;
		bool unproject;

		SerialiseGeometry(Value *obj, Document::AllocatorType *alloc, bool unproject) : 
			obj(obj), alloc(alloc), unproject(unproject) {}

		double deg2rad(double deg) { return (M_PI/180.0) * deg; }
		double rad2deg(double rad) { return (180.0/M_PI) * rad; }
		double latp2lat(double latp) { return rad2deg(atan(exp(deg2rad(latp)))*2.0)-90.0; }
		Value ringToArray(Ring &r) {
			Value arr(kArrayType);
			for (auto &point : r) {
				Value pt(kArrayType);
				pt.PushBack(point.x(), *alloc);
				pt.PushBack(unproject ? latp2lat(point.y()) : point.y(), *alloc);
				arr.PushBack(pt, *alloc);
			}
			return arr;
		}
		void operator()(Point &p) {} // todo
		void operator()(Linestring &ls) {} // todo
		void operator()(MultiLinestring &mls) {} // todo
		void operator()(Ring &r) {
			Value coordinates(kArrayType);
			coordinates.PushBack(ringToArray(r), *alloc);
			obj->AddMember("coordinates", coordinates, *alloc);
			obj->AddMember("type", Value().SetString("Polygon"), *alloc);
		}
		void operator()(Polygon &p) {
			Value coordinates(kArrayType);
			coordinates.PushBack(ringToArray(p.outer()), *alloc);
			for (auto &inner : p.inners()) {
				coordinates.PushBack(ringToArray(inner), *alloc);
			}
			obj->AddMember("coordinates", coordinates, *alloc);
			obj->AddMember("type", Value().SetString("Polygon"), *alloc);
		}
		void operator()(MultiPolygon &mp) {
			Value coordinates(kArrayType);
			for (auto &polygon : mp) {
				Value polyCoords(kArrayType);
				polyCoords.PushBack(ringToArray(polygon.outer()), *alloc);
				for (auto &inner : polygon.inners()) {
					polyCoords.PushBack(ringToArray(inner), *alloc);
				}
				coordinates.PushBack(polyCoords, *alloc);
			}
			obj->AddMember("coordinates", coordinates, *alloc);
			obj->AddMember("type", Value().SetString("MultiPolygon"), *alloc);
		}
	};
	void finalise(bool unproject = false) {
		Value features(kArrayType);
		for (AnyGeometry &g : geometries) {
			// type
			Value obj(kObjectType);
			obj.AddMember("type", Value().SetString("Feature"), document.GetAllocator());
			// properties (todo)
			Value properties(kObjectType);
			obj.AddMember("properties", properties, document.GetAllocator());
			// geometry
			Value geometry(kObjectType);
			boost::apply_visitor(SerialiseGeometry(&geometry, &(document.GetAllocator()), unproject), g);
			obj.AddMember("geometry", geometry, document.GetAllocator());
			// add to list
			features.PushBack(obj, document.GetAllocator());
		}
		document.AddMember("features", features, document.GetAllocator());
		geometries.clear();
	}
	std::string toString() {
		StringBuffer buffer;
		Writer<StringBuffer> writer(buffer);
		writer.SetMaxDecimalPlaces(5);
		document.Accept(writer);
		std::string json(buffer.GetString(), buffer.GetSize());
		return json;
	}
	void toFile(std::string filename) {
		auto fp = std::fopen(filename.c_str(), "w");
		char writeBuffer[65536];
		FileWriteStream os(fp, writeBuffer, sizeof(writeBuffer));
		Writer<FileWriteStream> writer(os);
		writer.SetMaxDecimalPlaces(5);
		document.Accept(writer);
		fclose(fp);
	}
};

#endif //_GEOJSON_WRITER_H
