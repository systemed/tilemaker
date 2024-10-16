#include "geom.h"
#include <iostream>

// Adapted from
// https://github.com/paulmach/orb/blob/dcade4901baea0727377ccf7c4aab2addd92d152/simplify/visvalingam.go
// and from
// https://github.com/felt/tippecanoe/blob/main/visvalingam.cpp

// The MIT License (MIT)
//
// Copyright (c) 2017 Paul Mach
//
// Permission is hereby granted, free of charge, to any person obtaining
// a copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject
// to the following conditions:
//
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
// IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
// ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
// CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#include <vector>
#include <cmath>

// Stuff to create the priority queue, or min heap.
// Rewriting it here, vs using the std lib, resulted in a 50% performance bump!

struct visItem {
	double area = 0;     // triangle area
	int pointIndex = 0;  // index of point in original path

	// to keep a virtual linked list to help rebuild the triangle areas as we remove points.
	visItem *next = NULL;
	visItem *previous = NULL;

	int index = 0;	// interal index in heap, for removal and update
};

struct minHeap {
	std::vector<visItem *> h;

	void Push(visItem *item) {
		item->index = h.size();
		h.push_back(item);
		up(item->index);
	}

	visItem *Pop() {
		visItem *removed = h[0];
		visItem *lastItem = h[h.size() - 1];
		h.pop_back();

		if (h.size() > 0) {
			lastItem->index = 0;
			h[0] = lastItem;
			down(0);
		}

		return removed;
	}

	void Update(visItem *item, double area) {
		if (item->area > area) {
			// area got smaller
			item->area = area;
			up(item->index);
		} else {
			// area got larger
			item->area = area;
			down(item->index);
		}
	}

	void up(int i) {
		visItem *object = h[i];
		while (i > 0) {
			int up = ((i + 1) >> 1) - 1;
			visItem *parent = h[up];

			if (parent->area <= object->area) {
				// parent is smaller so we're done fixing up the heap.
				break;
			}

			// swap nodes
			parent->index = i;
			h[i] = parent;

			object->index = up;
			h[up] = object;

			i = up;
		}
	}

	void down(int i) {
		visItem *object = h[i];
		while (1) {
			size_t right = (i + 1) << 1;
			size_t left = right - 1;

			int down = i;
			visItem *child = h[down];

			// swap with smallest child
			if (left < h.size() && h[left]->area < child->area) {
				down = left;
				child = h[down];
			}

			if (right < h.size() && h[right]->area < child->area) {
				down = right;
				child = h[down];
			}

			// non smaller, so quit
			if (down == i) {
				break;
			}

			// swap the nodes
			child->index = i;
			h[child->index] = child;

			object->index = down;
			h[down] = object;

			i = down;
		}
	}
};

template<typename GeometryType>
static double doubleTriangleArea(GeometryType const &ls, int start, int i1, int i2, int i3) {
	Point a = ls[i1 + start];
	Point b = ls[i2 + start];
	Point c = ls[i3 + start];

	return std::abs((b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x()));
}

template<typename GeometryType>
GeometryType visvalingam(const GeometryType &ls, double threshold, size_t retain) {
	size_t start = 0;
	size_t end = ls.size(); // - 1;
	int removed = 0;
	threshold *= 2;

	// build the initial minheap linked list.
	minHeap heap;

	visItem linkedListStart;
	linkedListStart.area = INFINITY;
	linkedListStart.pointIndex = 0;
	heap.Push(&linkedListStart);

	// internal path items
	std::vector<visItem> items;
	items.resize((end - start));

	{
		visItem *previous = &linkedListStart;
		for (size_t i = 1; i < (end - start) - 1; i++) {
			visItem *item = &items[i];

			item->area = doubleTriangleArea(ls, start, i - 1, i, i + 1);
			item->pointIndex = i;
			item->previous = previous;

			heap.Push(item);
			previous->next = item;
			previous = item;
		}

		// final item
		visItem *endItem = &items[(end - start) - 1];
		endItem->area = INFINITY;
		endItem->pointIndex = (end - start) - 1;
		endItem->previous = previous;

		previous->next = endItem;
		heap.Push(endItem);
	}

	// run through the reduction process
	while (heap.h.size() > 0) {
		visItem *current = heap.Pop();
		if (current->area > threshold) {
			break;
		} else if ((end - start) - removed <= retain) {
			break;
		}

		visItem *next = current->next;
		visItem *previous = current->previous;

		// remove current element from linked list
		previous->next = current->next;
		next->previous = current->previous;
		removed++;

		// figure out the new areas
		if (previous->previous != NULL) {
			double area = doubleTriangleArea(ls, start,
							 previous->previous->pointIndex,
							 previous->pointIndex,
							 next->pointIndex);

			area = std::max(area, current->area);
			heap.Update(previous, area);
		}

		if (next->next != NULL) {
			double area = doubleTriangleArea(ls, start,
							 previous->pointIndex,
							 next->pointIndex,
							 next->next->pointIndex);

			area = std::max(area, current->area);
			heap.Update(next, area);
		}
	}

	GeometryType output;
	visItem *item = &linkedListStart;
	while (item != NULL) {
		output.emplace_back(ls[item->pointIndex + start]);
		item = item->next;
	}
	return output;
}

Linestring simplifyVis(const Linestring &ls, double max_distance) {
	if (ls.size()<3) return ls;
	bool isClosed = ls[0].x()==ls[ls.size()-1].x() && ls[0].y()==ls[ls.size()-1].y();
	double threshold = max_distance * max_distance * 4;
	return visvalingam(ls, threshold, isClosed ? 3 : 2);
}
Polygon simplifyVis(const Polygon &p, double max_distance) { 
	Polygon output;
	double threshold = max_distance * max_distance * 4;
	output.outer() = visvalingam(p.outer(), threshold, 4);
	for (const auto &ring : p.inners()) {
		output.inners().emplace_back(visvalingam(ring, threshold, 4));
	}
	return output;
}
MultiPolygon simplifyVis(const MultiPolygon &mp, double max_distance) { 
	MultiPolygon output;
	for (const auto &p : mp) {
		output.emplace_back(simplifyVis(p, max_distance));
	}
	make_valid(output);
	return output;
}
