#ifndef _WRITE_GEOMETRY_H
#define _WRITE_GEOMETRY_H

#include <vector>
#include <utility>
#include <boost/variant.hpp>
#include "coordinates.h"

// Protobuf
#include "osmformat.pb.h"
#include "vector_tile.pb.h"

/*
	WriteGeometryVisitor
	- takes a boost::geometry object and writes it into a tile
*/

typedef std::vector<std::pair<int,int> > XYString;

class WriteGeometryVisitor : public boost::static_visitor<> { 

public:
	TileBbox *bboxPtr;
	vector_tile::Tile_Feature *featurePtr;
	double simplifyLevel;

	WriteGeometryVisitor(TileBbox *bp, vector_tile::Tile_Feature *fp, double sl);

	// Point
	void operator()(Point &p) const;

	// Multipolygon
	void operator()(MultiPolygon &mp) const;

	// Multilinestring
	void operator()(MultiLinestring &mls) const;

	// Linestring
	void operator()(Linestring &ls) const;

	// Encode a series of pixel co-ordinates into the feature, using delta and zigzag encoding
	void writeDeltaString(XYString *scaledString, vector_tile::Tile_Feature *featurePtr, std::pair<int,int> *lastPos, bool closePath) const;
};

#endif //_WRITE_GEOMETRY_H
