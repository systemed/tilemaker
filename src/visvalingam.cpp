#include "geom.h"
#include "visvalingam.h"
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
#include <cstdint>
#ifdef TMOPT_VERIFY
#include <cstdlib>
#include <cstring>
#endif

#ifdef TMOPT_VERIFY
// The original pointer-based implementation, used to cross-check the indexed one.
namespace visOld {

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

	void Reserve(size_t size) {
		h.reserve(size);
	}

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

}
using visOld::visItem;
using visOld::minHeap;

#endif

// Stuff to create the priority queue, or min heap.
// Rewriting it here, vs using the std lib, resulted in a 50% performance bump!
//
// Items are referred to by index; the heap stores (area, item) pairs and
// heapPos[item] is the item's slot in the heap. The heap operations (and so
// the tie-breaking between equal areas) are the same as in the original
// pointer-based version.

namespace {

struct visHeapEntry {
	double area;  // triangle area
	int item;     // item index
};

struct visScratch {
	std::vector<visHeapEntry> h;
	std::vector<int> heapPos;  // item -> index in h
	std::vector<int32_t> next; // virtual linked list, -1 = none
	std::vector<int32_t> prev;

	void prepare(size_t numItems) {
		h.clear();
		h.reserve(numItems);
		heapPos.resize(numItems);
		next.resize(numItems);
		prev.resize(numItems);
	}

	// Don't keep huge buffers alive after an unusually large ring.
	void trim() {
		const size_t cap = 1 << 16;
		if (h.capacity() > cap) {
			std::vector<visHeapEntry>().swap(h);
			std::vector<int>().swap(heapPos);
			std::vector<int32_t>().swap(next);
			std::vector<int32_t>().swap(prev);
		}
	}

	void Push(int item, double area) {
		int i = h.size();
		heapPos[item] = i;
		h.push_back(visHeapEntry{area, item});
		up(i);
	}

	visHeapEntry Pop() {
		visHeapEntry removed = h[0];
		visHeapEntry lastItem = h[h.size() - 1];
		h.pop_back();

		if (h.size() > 0) {
			heapPos[lastItem.item] = 0;
			h[0] = lastItem;
			down(0);
		}

		return removed;
	}

	void Update(int item, double area) {
		int i = heapPos[item];
		if (h[i].area > area) {
			// area got smaller
			h[i].area = area;
			up(i);
		} else {
			// area got larger
			h[i].area = area;
			down(i);
		}
	}

	void up(int i) {
		visHeapEntry object = h[i];
		while (i > 0) {
			int up = ((i + 1) >> 1) - 1;
			const visHeapEntry parent = h[up];

			if (parent.area <= object.area) {
				// parent is smaller so we're done fixing up the heap.
				break;
			}

			// swap nodes
			heapPos[parent.item] = i;
			h[i] = parent;

			heapPos[object.item] = up;
			h[up] = object;

			i = up;
		}
	}

	void down(int i) {
		visHeapEntry object = h[i];
		const size_t size = h.size();
		while (1) {
			size_t right = (i + 1) << 1;
			size_t left = right - 1;

			int down = i;
			double childArea = h[down].area;

			// swap with smallest child
			if (left < size && h[left].area < childArea) {
				down = left;
				childArea = h[down].area;
			}

			if (right < size && h[right].area < childArea) {
				down = right;
				childArea = h[down].area;
			}

			// non smaller, so quit
			if (down == i) {
				break;
			}

			// swap the nodes
			const visHeapEntry child = h[down];
			heapPos[child.item] = i;
			h[i] = child;

			heapPos[object.item] = down;
			h[down] = object;

			i = down;
		}
	}
};

thread_local visScratch visTls;

}

template<typename GeometryType>
static double doubleTriangleArea(GeometryType const &ls, int start, int i1, int i2, int i3) {
	Point const &a = ls[i1 + start];
	Point const &b = ls[i2 + start];
	Point const &c = ls[i3 + start];

	return std::abs((b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x()));
}

#ifdef TMOPT_VERIFY
template<typename GeometryType>
GeometryType visvalingamOld(const GeometryType &ls, double threshold, size_t retain) {
	size_t start = 0;
	size_t end = ls.size(); // - 1;
	int removed = 0;
	threshold *= 2;

	// build the initial minheap linked list.
	minHeap heap;
	heap.Reserve(end - start);

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
	output.reserve(end - start - removed);
	visItem *item = &linkedListStart;
	while (item != NULL) {
		output.emplace_back(ls[item->pointIndex + start]);
		item = item->next;
	}
	return output;
}
#endif

template<typename GeometryType>
GeometryType visvalingam(const GeometryType &ls, double threshold, size_t retain) {
	size_t start = 0;
	size_t end = ls.size(); // - 1;
	int removed = 0;
	threshold *= 2;
	if (ls.empty()) return ls;

	const int n = end - start;
	// Items 0..n-1 are the path points; item n is the list head, which
	// (like the original's separate linkedListStart) refers to point 0.
	const int head = n;
	auto pointIndex = [head](int item) { return item == head ? 0 : item; };

	visScratch &s = visTls;
	s.prepare(n + 1);
	std::vector<int32_t> &next = s.next;
	std::vector<int32_t> &prev = s.prev;

	// build the initial minheap linked list.
	prev[head] = -1;
	s.Push(head, INFINITY);

	{
		int previous = head;
		for (int i = 1; i < n - 1; i++) {
			prev[i] = previous;
			s.Push(i, doubleTriangleArea(ls, start, i - 1, i, i + 1));
			next[previous] = i;
			previous = i;
		}

		// final item
		int endItem = n - 1;
		prev[endItem] = previous;
		next[endItem] = -1;

		next[previous] = endItem;
		s.Push(endItem, INFINITY);
	}

	// run through the reduction process
	while (s.h.size() > 0) {
		visHeapEntry current = s.Pop();
		if (current.area > threshold) {
			break;
		} else if ((end - start) - removed <= retain) {
			break;
		}

		int nextItem = next[current.item];
		int previous = prev[current.item];

		// remove current element from linked list
		next[previous] = nextItem;
		prev[nextItem] = previous;
		removed++;

		// figure out the new areas
		if (prev[previous] != -1) {
			double area = doubleTriangleArea(ls, start,
							 pointIndex(prev[previous]),
							 pointIndex(previous),
							 pointIndex(nextItem));

			area = std::max(area, current.area);
			s.Update(previous, area);
		}

		if (next[nextItem] != -1) {
			double area = doubleTriangleArea(ls, start,
							 pointIndex(previous),
							 pointIndex(nextItem),
							 pointIndex(next[nextItem]));

			area = std::max(area, current.area);
			s.Update(nextItem, area);
		}
	}

	GeometryType output;
	output.reserve(end - start - removed);
	int item = head;
	while (item != -1) {
		output.emplace_back(ls[pointIndex(item) + start]);
		item = next[item];
	}
	s.trim();
#ifdef TMOPT_VERIFY
	{
		GeometryType expected = visvalingamOld(ls, threshold / 2, retain);
		bool same = expected.size() == output.size();
		for (size_t k = 0; same && k < output.size(); k++) {
			double a[2] = { output[k].x(), output[k].y() };
			double b[2] = { expected[k].x(), expected[k].y() };
			same = std::memcmp(a, b, sizeof(a)) == 0;
		}
		if (!same) {
			std::cerr << "TMOPT_VERIFY: visvalingam mismatch, n=" << ls.size() << " out=" << output.size() << " expected=" << expected.size() << std::endl;
			std::abort();
		}
	}
#endif
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
	output.inners().reserve(p.inners().size());
	for (const auto &ring : p.inners()) {
		output.inners().emplace_back(visvalingam(ring, threshold, 4));
	}
	return output;
}
MultiPolygon simplifyVis(const MultiPolygon &mp, double max_distance) {
	bool allValid;
	return simplifyVis(mp, max_distance, allValid);
}
// allValid: set to repair_multi_polygon's result, i.e. true when every polygon
// of the output is individually valid.
MultiPolygon simplifyVis(const MultiPolygon &mp, double max_distance, bool &allValid) {
	MultiPolygon output;
	output.reserve(mp.size());
	for (const auto &p : mp) {
		output.emplace_back(simplifyVis(p, max_distance));
	}
	// Per-ring simplification can leave the multipolygon invalid. Use the
	// area-preserving repair instead of a bare make_valid: on large/complex
	// geometries an unconditional dissolve can collapse the polygon and drop
	// almost all of its area (which showed up as missing lake tiles at low
	// zoom). repair_multi_polygon keeps the simplified geometry untouched if a
	// repair would not preserve the covered area.
	allValid = repair_multi_polygon(output);
	return output;
}
