#ifndef _COORDINATES_GEOM_H
#define _COORDINATES_GEOM_H

#include "coordinates.h"
#include "geom.h"

void insertIntermediateTiles(Linestring const &points, uint baseZoom, std::unordered_set<TileCoordinates> &tileSet);
void insertIntermediateTiles(Ring const &points, uint baseZoom, std::unordered_set<TileCoordinates> &tileSet);
// Vector variants: append the tiles to `tiles`, then sort and dedupe the appended range [old size, end).
void insertIntermediateTiles(Linestring const &points, uint baseZoom, std::vector<TileCoordinates> &tiles);
void insertIntermediateTiles(Ring const &points, uint baseZoom, std::vector<TileCoordinates> &tiles);

// ------------------------------------------------------
// Helper class for dealing with spherical Mercator tiles
class TileBbox { 

public:
	double minLon, maxLon, minLat, maxLat, minLatp, maxLatp;
	double xmargin, ymargin, xscale, yscale;
	TileCoordinates index;
	uint zoom;
	bool hires;
	bool endZoom;
	Box clippingBox;

	TileBbox(TileCoordinates i, uint z, bool h, bool e);

	std::pair<int,int> scaleLatpLon(double latp, double lon) const;
	void scaleRing(Ring &dst, Ring const &src) const;
	Ring scaleRing(Ring const &src) const;
	void scaleRingNoBacktrack(Ring &dst, Ring const &src) const;
	void scaleGeometry(MultiPolygon &dst, MultiPolygon const &src) const;
	MultiPolygon scaleGeometry(MultiPolygon const &src) const;
	std::pair<double, double> floorLatpLon(double latp, double lon) const;

	Box getTileBox() const;
	Box getExtendBox() const;
};


#endif
