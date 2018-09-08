/*! \file */ 
#ifndef _WRITE_GEOMETRY_H
#define _WRITE_GEOMETRY_H

#include <vector>
#include <utility>
#include <boost/variant.hpp>
#include "coordinates.h"

// Protobuf
#include "osmformat.pb.h"
#include "vector_tile.pb.h"

typedef std::vector<std::pair<int,int> > XYString;

/**
	\brief WriteGeometryVisitor takes a boost::geometry object and writes it into a tile
*/
class WriteGeometryVisitor : public boost::static_visitor<> { 

public:
	const TileBbox *bboxPtr;
	vector_tile::Tile_Feature *featurePtr;
	double simplifyLevel;

	WriteGeometryVisitor(const TileBbox *bp, vector_tile::Tile_Feature *fp, double sl);

	// Point
	void operator()(const Point &p) const;

	// Multipolygon
	void operator()(const MultiPolygon &mp) const;

	// Multilinestring
	void operator()(const MultiLinestring &mls) const;

	// Linestring
	void operator()(const Linestring &ls) const;

	/// \brief Encode a series of pixel co-ordinates into the feature, using delta and zigzag encoding
	void writeDeltaString(XYString *scaledString, vector_tile::Tile_Feature *featurePtr, std::pair<int,int> *lastPos, bool closePath) const;
};

#endif //_WRITE_GEOMETRY_H
