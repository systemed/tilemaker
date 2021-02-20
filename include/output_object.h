/*! \file */ 
#ifndef _OUTPUT_OBJECT_H
#define _OUTPUT_OBJECT_H

#include <vector>
#include <string>
#include <map>
#include <memory>
#include "geomtypes.h"
#include "coordinates.h"
#include "attribute_store.h"
#include "osm_store.h"

#include "clipper.hpp"

// Protobuf
#include "osmformat.pb.h"
#include "vector_tile.pb.h"


///\brief Specifies geometry type for an OutputObject
enum OutputGeometryType { POINT, LINESTRING, POLYGON, CENTROID, CACHED_POINT, CACHED_LINESTRING, CACHED_POLYGON };

class ClipGeometryVisitor : public boost::static_visitor<Geometry> {

	const Box &clippingBox; //for boost ggl
	ClipperLib::Path clippingPath; //for clipper library

public:
	ClipGeometryVisitor(const Box &cbox);

	Geometry operator()(const Point &p) const;

	Geometry operator()(const Linestring &ls) const;

	Geometry operator()(const MultiLinestring &mls) const;

	Geometry operator()(const MultiPolygon &mp) const;
};

/**
 * \brief OutputObject - any object (node, linestring, polygon) to be outputted to tiles

 * Possible future improvements to save memory:
 * - use a global dictionary for attribute key/values
*/
class OutputObject { 

public:
	OutputGeometryType geomType : 3;					// point, linestring, polygon...
	bool fromShapefile;									// from shapefile or pbf?
	unsigned minZoom : 4;
	uint_least8_t layer;								// what layer is it in?
	NodeID objectID;									// id of way (linestring/polygon) or node (point)
	std::vector<unsigned> attributeList;				// ids within attribute_store

	OutputObject(OutputGeometryType type, bool shp, uint_least8_t l, NodeID id);
	virtual ~OutputObject();	

	void setMinZoom(unsigned z);

	void addAttribute(unsigned attrIndex);

	/** \brief Assemble a linestring or polygon into a Boost geometry, and clip to bounding box
	 * Returns a boost::variant -
	 *	 POLYGON->MultiPolygon, CENTROID->Point, LINESTRING->MultiLinestring
	 */
	virtual Geometry buildWayGeometry(OSMStore &osmStore, const TileBbox &bbox) const = 0;
	
	///\brief Add a node geometry
	virtual LatpLon buildNodeGeometry(OSMStore &osmStore, const TileBbox &bbox) const = 0;
	
	///\brief Check if the object intersects with the given point
	virtual bool intersects(OSMStore &osmStore, Point const &p) const = 0;

	//\brief Write attribute key/value pairs (dictionary-encoded)
	void writeAttributes(std::vector<std::string> *keyList, 
		std::vector<vector_tile::Tile_Value> *valueList, vector_tile::Tile_Feature *featurePtr,
		const AttributeStore &attributeStore) const;
	
	/**
	 * \brief Find a value in the value dictionary
	 * (we can't easily use find() because of the different value-type encoding - 
	 *	should be possible to improve this though)
	 */
	int findValue(std::vector<vector_tile::Tile_Value> *valueList, vector_tile::Tile_Value *value) const;
};

/**
 * \brief An OutputObject derived class that contains data originally from OsmMemTiles
*/
class OutputObjectOsmStorePoint : public OutputObject
{
public:
	OutputObjectOsmStorePoint(OutputGeometryType type, uint_least8_t l, NodeID id, std::size_t index)
		: OutputObject(type, false, l, id), index(index)
	{ 
		assert(type == POINT || type == CENTROID || type == CACHED_POINT);
	}

	Geometry buildWayGeometry(OSMStore &osmStore, const TileBbox &bbox) const
	{
		auto &p = osmStore.retrieve_point(index);
		if (geom::within(p, bbox.clippingBox)) {
			return p;
		} 
		return MultiLinestring();
	}

	LatpLon buildNodeGeometry(OSMStore &osmStore, const TileBbox &bbox) const
	{
		auto const &pt = osmStore.retrieve_point(index);
		LatpLon out;
		out.latp = pt.y();
		out.lon = pt.x();
		return out;
	}

	bool intersects(OSMStore &osmStore, Point const &p) const
	{
		return boost::geometry::intersects(osmStore.retrieve_point(index), p);
	}

private:
	std::size_t index;
}; 

class OutputObjectOsmStoreLinestring : public OutputObject
{
public:
	OutputObjectOsmStoreLinestring(OutputGeometryType type, uint_least8_t l, NodeID id, std::size_t index)
		: OutputObject(type, false, l, id), index(index)
	{ 
		assert(type == LINESTRING || type == CACHED_LINESTRING);
	}

	Geometry buildWayGeometry(OSMStore &osmStore, const TileBbox &bbox) const
	{
		MultiLinestring out;
		geom::intersection(osmStore.retrieve_linestring(index), bbox.clippingBox, out);
		return out;
	}

	LatpLon buildNodeGeometry(OSMStore &osmStore, const TileBbox &bbox) const
	{
		throw std::runtime_error("Geometry type is not point");
	}

	bool intersects(OSMStore &osmStore, Point const &p) const 
	{
		return boost::geometry::intersects(osmStore.retrieve_linestring(index), p);
	}

private:
	std::size_t index;
};

class OutputObjectOsmStoreMultiPolygon : public OutputObject
{
public:
	OutputObjectOsmStoreMultiPolygon(OutputGeometryType type, uint_least8_t l, NodeID id, std::size_t index)
		: OutputObject(type, false, l, id), index(index)
	{ 
		assert(type == POLYGON || type == CACHED_POLYGON);
	}

	Geometry buildWayGeometry(OSMStore &osmStore, const TileBbox &bbox) const
	{
		auto mp = osmStore.retrieve_multi_polygon(index);

		Polygon clippingPolygon;

		geom::convert(bbox.clippingBox, clippingPolygon);
		if (!geom::intersects(mp, clippingPolygon)) { return MultiPolygon(); }
		if (geom::within(mp, clippingPolygon)) { 
			MultiPolygon out;
			boost::geometry::assign(out, mp);
			return out; 
		}

		try {
			MultiPolygon out;
			geom::intersection(mp, clippingPolygon, out);
			return out;
		} catch (geom::overlay_invalid_input_exception &err) {
			std::cout << "Couldn't clip polygon (self-intersection)" << std::endl;
			return MultiPolygon(); // blank
		}
	}

	LatpLon buildNodeGeometry(OSMStore &osmStore, const TileBbox &bbox) const
	{
		throw std::runtime_error("Geometry type is not point");
	}

	bool intersects(OSMStore &osmStore, Point const &p) const
	{
		return boost::geometry::intersects(osmStore.retrieve_multi_polygon(index), p);
	}

private:
	std::size_t index;
};

typedef std::shared_ptr<OutputObject> OutputObjectRef;

// Comparison functions

bool operator==(const OutputObjectRef &x, const OutputObjectRef &y);

/**
 * Do lexicographic comparison, with the order of: layer, geomType, attributes, and objectID.
 * Note that attributes is preferred to objectID.
 * It is to arrange objects with the identical attributes continuously.
 * Such objects will be merged into one object, to reduce the size of output.
 */
bool operator<(const OutputObjectRef &x, const OutputObjectRef &y);

namespace vector_tile {
	bool operator==(const vector_tile::Tile_Value &x, const vector_tile::Tile_Value &y);
	bool operator<(const vector_tile::Tile_Value &x, const vector_tile::Tile_Value &y);
}

namespace std {
	/// Hashing function so we can use an unordered_set
	template<>
	struct hash<OutputObjectRef> {
		size_t operator()(const OutputObjectRef &oo) const {
			return std::hash<uint_least8_t>()(oo->layer) ^ std::hash<NodeID>()(oo->objectID);
		}
	};
}

#endif //_OUTPUT_OBJECT_H
