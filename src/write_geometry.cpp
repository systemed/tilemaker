#include "write_geometry.h"
#include "helpers.h"
using namespace std;
namespace geom = boost::geometry;

WriteGeometryVisitor::WriteGeometryVisitor(const TileBbox *bp, vector_tile::Tile_Feature *fp, double sl) {
	bboxPtr = bp;
	featurePtr = fp;
	simplifyLevel = sl;
}

// Point
void WriteGeometryVisitor::operator()(const Point &p) const {
	if (geom::within(p, bboxPtr->clippingBox)) {
		featurePtr->add_geometry(9);					// moveTo, repeat x1
		pair<int,int> xy = bboxPtr->scaleLatpLon(p.y(), p.x());
		featurePtr->add_geometry((xy.first  << 1) ^ (xy.first  >> 31));
		featurePtr->add_geometry((xy.second << 1) ^ (xy.second >> 31));
		featurePtr->set_type(vector_tile::Tile_GeomType_POINT);
	}
}

// Multipolygon
void WriteGeometryVisitor::operator()(const MultiPolygon &mp) const {
	MultiPolygon current;
	if (simplifyLevel>0)
	{
		//geom::simplify is glitchy and sometimes breaks polygons. For now,
		//disable polygon simplification. https://github.com/systemed/tilemaker/issues/131
		//geom::simplify(mp, current, simplifyLevel);

		ClipperSimplify(mp, simplifyLevel, current);
	}
	else
		current = mp;

	pair<int,int> lastPos(0,0);
	for (MultiPolygon::const_iterator it = current.begin(); it != current.end(); ++it) {
		XYString scaledString;
		Ring ring = geom::exterior_ring(*it);
		for (auto jt = ring.begin(); jt != ring.end(); ++jt) {
			pair<int,int> xy = bboxPtr->scaleLatpLon(jt->get<1>(), jt->get<0>());
			scaledString.push_back(xy);
		}
		writeDeltaString(&scaledString, featurePtr, &lastPos, true);

		InteriorRing interiors = geom::interior_rings(*it);
		for (auto ii = interiors.begin(); ii != interiors.end(); ++ii) {
			scaledString.clear();
			XYString scaledInterior;
			for (auto jt = ii->begin(); jt != ii->end(); ++jt) {
				pair<int,int> xy = bboxPtr->scaleLatpLon(jt->get<1>(), jt->get<0>());
				scaledString.push_back(xy);
			}
			writeDeltaString(&scaledString, featurePtr, &lastPos, true);
		}
	}
	featurePtr->set_type(vector_tile::Tile_GeomType_POLYGON);
}

// Multilinestring
void WriteGeometryVisitor::operator()(const MultiLinestring &mls) const {
	MultiLinestring current;
	if (simplifyLevel>0)
		geom::simplify(mls, current, simplifyLevel);
	else 
		current = mls;

	pair<int,int> lastPos(0,0);
	for (MultiLinestring::const_iterator it = current.begin(); it != current.end(); ++it) {
		XYString scaledString;
		for (Linestring::const_iterator jt = it->begin(); jt != it->end(); ++jt) {
			pair<int,int> xy = bboxPtr->scaleLatpLon(jt->get<1>(), jt->get<0>());
			scaledString.push_back(xy);
		}
		writeDeltaString(&scaledString, featurePtr, &lastPos, false);
	}
	featurePtr->set_type(vector_tile::Tile_GeomType_LINESTRING);
}

// Linestring
void WriteGeometryVisitor::operator()(const Linestring &ls) const { 
	Linestring current;
	if (simplifyLevel>0)
		geom::simplify(ls, current, simplifyLevel);
	else
		current = ls;

	pair<int,int> lastPos(0,0);
	XYString scaledString;
	for (Linestring::const_iterator jt = current.begin(); jt != current.end(); ++jt) {
		pair<int,int> xy = bboxPtr->scaleLatpLon(jt->get<1>(), jt->get<0>());
		scaledString.push_back(xy);
	}
	writeDeltaString(&scaledString, featurePtr, &lastPos, false);
	featurePtr->set_type(vector_tile::Tile_GeomType_LINESTRING);
}

// Encode a series of pixel co-ordinates into the feature, using delta and zigzag encoding
void WriteGeometryVisitor::writeDeltaString(XYString *scaledString, vector_tile::Tile_Feature *featurePtr, pair<int,int> *lastPos, bool closePath) const {
	if (scaledString->size()<2) return;
	vector<uint32_t> geometry;

	// Start with a moveTo
	int lastX = scaledString->at(0).first;
	int lastY = scaledString->at(0).second;
	int dx = lastX - lastPos->first;
	int dy = lastY - lastPos->second;
	geometry.push_back(9);						// moveTo, repeat x1
	geometry.push_back((dx << 1) ^ (dx >> 31));
	geometry.push_back((dy << 1) ^ (dy >> 31));

	// Then write out the line for each point
	uint len=0;
	geometry.push_back(0);						// this'll be our lineTo opcode, we set it later
	for (uint i=1; i<scaledString->size(); i++) {
		int x = scaledString->at(i).first;
		int y = scaledString->at(i).second;
		if (x==lastX && y==lastY) { continue; }
		dx = x-lastX;
		dy = y-lastY;
		geometry.push_back((dx << 1) ^ (dx >> 31));
		geometry.push_back((dy << 1) ^ (dy >> 31));
		lastX = x; lastY = y;
		len++;
	}
	if (len==0) return;
	geometry[3] = (len << 3) + 2;				// lineTo plus repeat
	if (closePath) {
		geometry.push_back(7+8);				// closePath
	}
	for (uint i=0; i<geometry.size(); i++) { 
		featurePtr->add_geometry(geometry[i]);
	};
	lastPos->first  = lastX;
	lastPos->second = lastY;
}

