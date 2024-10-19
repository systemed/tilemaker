/*! \file */ 
#ifndef _VISVALINGAM_H
#define _VISVALINGAM_H

// Visvalingam simplify
Linestring simplifyVis(const Linestring &ls, double max_distance);
Polygon simplifyVis(const Polygon &p, double max_distance);
MultiPolygon simplifyVis(const MultiPolygon &mp, double max_distance);

#endif //_VISVALINGAM_H
