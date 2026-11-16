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
#include <cstring>
#include <algorithm>


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


// The original implementation; visvalingam() below gives the same results
// faster and uses this one for the degenerate cases.
template<typename GeometryType>
static GeometryType visvalingamReference(const GeometryType &ls, double threshold, size_t retain) {
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
	return output;
}

// Faster implementation of the same algorithm. It makes exactly the same
// removals in the same order as visvalingamReference, so its output is the
// same for every input (that is checked against recorded planet inputs):
//  - The heap is the same binary heap. Every operation leaves every slot
//    holding the same (area, item) entry as the reference's operation does,
//    so equal areas are popped in the same order:
//    - sifts move a hole instead of swapping (same final slots);
//    - down(): "the smaller child, the left one on a tie, if it is smaller
//      than the entry" equals the reference's "left if smaller than the
//      entry, then right if smaller than that" (needs NaN-free areas, see
//      the coordinate check below);
//    - Pop() moves the hole along that smaller-child path to a leaf and then
//      lets the last entry rise while its parent is >= it. The areas along
//      the path never decrease, so the entry ends below exactly the path
//      entries smaller than it: the slot the reference's down() gives it.
//      Two levels per step: the grandchild pairs are compared together with
//      the children, then the pair below the chosen child is used;
//    - Update() to an unchanged area moves nothing in the reference either
//      (no child is smaller), so it is skipped;
//    - while the heap is built only Push() runs, so pos[] is filled once
//      after the pushes.
//  - Items are numbered like points (the reference's separate list-head item
//    n becomes item 0, which the reference never uses); only the item numbers
//    in the heap entries change, never a comparison.
//  - The stop tests are made on the heap top before popping it.
//  - Inputs where nothing can be removed (n <= retain, or every initial
//    triangle area above the threshold) return ls without building the heap.
//  - The output is the linked list in order, as in the reference.

namespace {

struct visFastEntry {
	double area;
	int32_t item;
};

struct visFastLink {
	int32_t prev, next; // linked list, -1 = none
};

struct visFastScratch {
	std::vector<visFastEntry> h;
	std::vector<int32_t> pos;    // item -> slot in the heap
	std::vector<visFastLink> link;
	std::vector<double> area;    // initial triangle areas

	void prepare(size_t n) {
		if (pos.size() < n) {
			h.resize(n + 4);
			pos.resize(n);
			link.resize(n);
			area.resize(n);
		}
	}

	// Heap storage starts at an address = 16 (mod 64): the children 2i+1, 2i+2
	// of a slot then share 32 bytes and its four grandchildren a cache line.
	visFastEntry *heapBase() {
		visFastEntry *p = h.data();
		while ((reinterpret_cast<uintptr_t>(p) & 63) != 16) p++;
		return p;
	}

	// Don't keep huge buffers alive after an unusually large ring.
	void trim() {
		if (pos.size() > (1 << 16)) {
			std::vector<visFastEntry>().swap(h);
			std::vector<int32_t>().swap(pos);
			std::vector<visFastLink>().swap(link);
			std::vector<double>().swap(area);
		}
	}
};

thread_local visFastScratch visFastTls;

struct visFastHeap {
	visFastEntry *h;
	int32_t *pos;
	size_t size = 0;

	void up(size_t i, visFastEntry object) {
		while (i > 0) {
			const size_t up = ((i + 1) >> 1) - 1;
			const visFastEntry parent = h[up];
			if (parent.area <= object.area) break;
			h[i] = parent;
			pos[parent.item] = i;
			i = up;
		}
		h[i] = object;
		pos[object.item] = i;
	}

	void down(size_t i, visFastEntry object) {
		while (1) {
			const size_t left = 2 * i + 1;
			if (left >= size) break;
			size_t c = left;
			if (left + 1 < size && h[left + 1].area < h[left].area) c = left + 1;
			const visFastEntry child = h[c];
			if (!(child.area < object.area)) break;
			h[i] = child;
			pos[child.item] = i;
			i = c;
		}
		h[i] = object;
		pos[object.item] = i;
	}

	// Push without maintaining pos (fixPos() before any other operation).
	void PushBuild(int32_t item, double area) {
		size_t i = size++;
		while (i > 0) {
			const size_t up = ((i + 1) >> 1) - 1;
			if (h[up].area <= area) break;
			h[i] = h[up];
			i = up;
		}
		h[i] = visFastEntry{area, item};
	}

	void fixPos() {
		for (size_t i = 0; i < size; i++) pos[h[i].item] = i;
	}

	void Pop() {
		const visFastEntry object = h[--size];
		if (size == 0) return;
		size_t i = 0;
		while (4 * i + 6 < size) {
			const size_t left = 2 * i + 1;
			const size_t g = 4 * i + 3;
			const size_t c = left + (h[left + 1].area < h[left].area);
			const size_t gl = g + (h[g + 1].area < h[g].area);
			const size_t gr = g + 2 + (h[g + 3].area < h[g + 2].area);
			const size_t gc = c == left ? gl : gr;
			const visFastEntry child = h[c];
			const visFastEntry grandchild = h[gc];
			h[i] = child;
			pos[child.item] = i;
			h[c] = grandchild;
			pos[grandchild.item] = c;
			i = gc;
		}
		while (1) {
			const size_t left = 2 * i + 1;
			if (left >= size) break;
			size_t c = left;
			if (left + 1 < size && h[left + 1].area < h[left].area) c = left + 1;
			const visFastEntry child = h[c];
			h[i] = child;
			pos[child.item] = i;
			i = c;
		}
		while (i > 0) {
			const size_t up = ((i + 1) >> 1) - 1;
			const visFastEntry parent = h[up];
			if (parent.area < object.area) break;
			h[i] = parent;
			pos[parent.item] = i;
			i = up;
		}
		h[i] = object;
		pos[object.item] = i;
	}

	void Update(int32_t item, double area) {
		const size_t i = pos[item];
		if (h[i].area > area) up(i, visFastEntry{area, item});
		else if (h[i].area < area) down(i, visFastEntry{area, item});
	}
};

}

static inline double doubleTriangleArea(const Point &a, const Point &b, const Point &c) {
	return std::abs((b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x()));
}

// Packed variant of visFastHeap for inputs with integral coordinates of
// bounded size (rings in tile coordinates): an entry is one uint64_t, the bit
// pattern of its area with the item number in the low K bits.
//  - For x, y >= 0 (not NaN), x < y as doubles exactly when their bit
//    patterns are in that order as unsigned integers; +infinity included.
//  - With |coordinate| < 2^((50 - K) / 2) every doubled triangle area is an
//    integer below 2^(53 - K) (|area| <= 8 max|c|^2), computed exactly as the
//    same double, and such a double has its K lowest mantissa bits zero, so
//    or-ing the item into them loses nothing (and infinity's are zero too).
//  - "area of a < area of b" is a < (b & ~M) for M = 2^K - 1: the masked
//    patterns differ by at least 2^K when the areas differ, so the item bits
//    of a never change the outcome; for equal areas it is false, as for the
//    doubles.
// So every comparison has the outcome visFastHeap's has, every operation
// moves the same entries to the same slots, and the removals are the same.
namespace {

struct visPackHeap {
	uint64_t *h;
	int32_t *pos;
	uint64_t hi; // ~M
	uint64_t M;
	size_t size = 0;

	bool lt(uint64_t a, uint64_t b) const { return a < (b & hi); } // area(a) < area(b)

	void up(size_t i, uint64_t object) {
		while (i > 0) {
			const size_t up = ((i + 1) >> 1) - 1;
			const uint64_t parent = h[up];
			if (!lt(object, parent)) break; // parent <= object
			h[i] = parent;
			pos[parent & M] = i;
			i = up;
		}
		h[i] = object;
		pos[object & M] = i;
	}

	void down(size_t i, uint64_t object) {
		while (1) {
			const size_t left = 2 * i + 1;
			if (left >= size) break;
			size_t c = left;
			if (left + 1 < size && lt(h[left + 1], h[left])) c = left + 1;
			const uint64_t child = h[c];
			if (!lt(child, object)) break;
			h[i] = child;
			pos[child & M] = i;
			i = c;
		}
		h[i] = object;
		pos[object & M] = i;
	}

	void PushBuild(uint64_t key) {
		size_t i = size++;
		while (i > 0) {
			const size_t up = ((i + 1) >> 1) - 1;
			if (!lt(key, h[up])) break;
			h[i] = h[up];
			i = up;
		}
		h[i] = key;
	}

	void fixPos() {
		for (size_t i = 0; i < size; i++) pos[h[i] & M] = i;
	}

	void Pop() {
		const uint64_t object = h[--size];
		if (size == 0) return;
		size_t i = 0;
		while (4 * i + 6 < size) {
			const size_t left = 2 * i + 1;
			const size_t g = 4 * i + 3;
			const size_t c = left + lt(h[left + 1], h[left]);
			const size_t gl = g + lt(h[g + 1], h[g]);
			const size_t gr = g + 2 + lt(h[g + 3], h[g + 2]);
			const size_t gc = c == left ? gl : gr;
			const uint64_t child = h[c];
			const uint64_t grandchild = h[gc];
			h[i] = child;
			pos[child & M] = i;
			h[c] = grandchild;
			pos[grandchild & M] = c;
			i = gc;
		}
		while (1) {
			const size_t left = 2 * i + 1;
			if (left >= size) break;
			size_t c = left;
			if (left + 1 < size && lt(h[left + 1], h[left])) c = left + 1;
			const uint64_t child = h[c];
			h[i] = child;
			pos[child & M] = i;
			i = c;
		}
		while (i > 0) {
			const size_t up = ((i + 1) >> 1) - 1;
			const uint64_t parent = h[up];
			if (lt(parent, object)) break;
			h[i] = parent;
			pos[parent & M] = i;
			i = up;
		}
		h[i] = object;
		pos[object & M] = i;
	}

	// areaBits: bit pattern of the new area
	void Update(int32_t item, uint64_t areaBits) {
		const size_t i = pos[item];
		const uint64_t cur = h[i] & hi;
		if (cur > areaBits) up(i, areaBits | uint64_t(item));
		else if (cur < areaBits) down(i, areaBits | uint64_t(item));
	}
};

struct visPackScratch {
	std::vector<uint64_t> h;
	std::vector<int32_t> pos;
	std::vector<visFastLink> link;

	void prepare(size_t n) {
		if (pos.size() < n) {
			h.resize(n + 8);
			pos.resize(n);
			link.resize(n);
		}
	}
	// heap storage at an address = 8 (mod 64): the children 2i+1, 2i+2 of a
	// slot share 16 bytes, its grandchildren 32 and great-grandchildren a line
	uint64_t *heapBase() {
		uint64_t *p = h.data();
		while ((reinterpret_cast<uintptr_t>(p) & 63) != 8) p++;
		return p;
	}
	void trim() {
		if (pos.size() > (1 << 16)) {
			std::vector<uint64_t>().swap(h);
			std::vector<int32_t>().swap(pos);
			std::vector<visFastLink>().swap(link);
		}
	}
};

thread_local visPackScratch visPackTls;

inline uint64_t visBits(double d) { uint64_t u; std::memcpy(&u, &d, 8); return u; }
inline double visDouble(uint64_t u) { double d; std::memcpy(&d, &u, 8); return d; }

}

// The same reduction as in visvalingamRun() below with visPackHeap; area[] holds
// the initial doubled areas, threshold is already doubled, K as above. Returns
// the linked list of the remaining points (from item 0).
template<typename GeometryType>
static const visFastLink *visvalingamPacked(const GeometryType &ls, double threshold, size_t retain, const double *area, unsigned K) {
	const size_t n = ls.size();
	visPackScratch &s = visPackTls;
	s.prepare(n);
	const Point *p = &ls[0];
	visFastLink *link = s.link.data();
	const uint64_t M = (uint64_t(1) << K) - 1;
	visPackHeap heap{s.heapBase(), s.pos.data(), ~M, M};
	const int32_t endItem = n - 1;
	const uint64_t inf = visBits(INFINITY);
	heap.PushBuild(inf | 0);
	for (size_t i = 1; i < n - 1; i++) heap.PushBuild(visBits(area[i]) | uint64_t(i));
	heap.PushBuild(inf | uint64_t(endItem));
	heap.fixPos();
	for (int32_t i = 0; i <= endItem; i++) link[i] = visFastLink{i - 1, i + 1};
	link[endItem].next = -1;

	size_t removed = 0;
	while (heap.size > 0) {
		const uint64_t current = heap.h[0];
		const uint64_t currentArea = current & ~M;
		if (visDouble(currentArea) > threshold) break;
		if (n - removed <= retain) break;
		heap.Pop();

		const int32_t item = int32_t(current & M);
		const visFastLink cl = link[item];
		const int32_t nextItem = cl.next;
		const int32_t prevItem = cl.prev;
		const int32_t prevPrev = link[prevItem].prev;
		const int32_t nextNext = link[nextItem].next;
		link[prevItem].next = nextItem;
		link[nextItem].prev = prevItem;
		removed++;

		// max of the areas = max of the bit patterns
		if (prevPrev != -1) {
			const uint64_t a = visBits(doubleTriangleArea(p[prevPrev], p[prevItem], p[nextItem]));
			heap.Update(prevItem, std::max(a, currentArea));
		}
		if (nextNext != -1) {
			const uint64_t a = visBits(doubleTriangleArea(p[prevItem], p[nextItem], p[nextNext]));
			heap.Update(nextItem, std::max(a, currentArea));
		}
	}

	return link;
}

// Runs the reduction. Returns the linked list of the remaining points (from
// item 0, in the thread's scratch space; visTrim() when done with it), or
// nullptr with status 0 when nothing is removed, 2 when the caller must use
// visvalingamReference.
template<typename GeometryType>
static const visFastLink *visvalingamRun(const GeometryType &ls, double threshold, size_t retain, int &status) {
	const size_t n = ls.size();
	status = 2;
	// Fewer than 3 points or retain < 2 (the reference's list head / end item
	// handling differs there), or coordinates so large that an area could
	// overflow to NaN (with |coordinate| <= 1e100 all areas are finite, which
	// the heap above needs).
	if (n < 3 || retain < 2) return nullptr;
	// K: bits for the item numbers 0..n-1 in visPackHeap; packable: every
	// coordinate is an integer of absolute value below lim (see visPackHeap)
	unsigned K = 1;
	while ((size_t(1) << K) < n) K++;
	// (small inputs are faster without it: the check costs more than it saves)
	bool packable = n >= 16 && K <= 40;
	if (packable) {
		const double lim = std::ldexp(1.0, (50 - int(K)) / 2);
		for (const Point &q : ls) {
			const double ax = std::abs(q.x()), ay = std::abs(q.y());
			// the conversions only see values in [-1, lim)
			const double cx = ax < lim ? ax : -1.0, cy = ay < lim ? ay : -1.0;
			if (!(double(int64_t(cx)) == ax && double(int64_t(cy)) == ay)) { packable = false; break; }
		}
	}
	// (packable implies every |coordinate| < lim <= 2^25)
	if (!packable) {
		bool small = true;
		for (const Point &q : ls) small &= std::abs(q.x()) <= 1e100 && std::abs(q.y()) <= 1e100;
		if (!small) return nullptr;
	}
	status = 0;
	threshold *= 2;
	// n <= retain: the first pop stops the reduction (by its area or by the
	// retain test), nothing is removed.
	if (n <= retain) return nullptr;

	visFastScratch &s = visFastTls;
	s.prepare(n);
	const Point *p = &ls[0];

	// If every initial area exceeds the threshold, so does the heap minimum
	// (the list head and end have area infinity, and the threshold is below
	// some finite area): the first pop stops the reduction.
	double *area = s.area.data();
	bool allAbove = true;
	for (size_t i = 1; i < n - 1; i++) {
		area[i] = doubleTriangleArea(p[i - 1], p[i], p[i + 1]);
		allAbove &= area[i] > threshold;
	}
	if (allAbove) return nullptr;
	status = 1;
	if (packable) return visvalingamPacked(ls, threshold, retain, area, K);

	// Item 0 is the list head (point 0), items 1..n-1 the other points, pushed
	// in the reference's order: head, 1..n-2, end.
	visFastLink *link = s.link.data();
	visFastHeap heap{s.heapBase(), s.pos.data()};
	const int32_t endItem = n - 1;
	heap.PushBuild(0, INFINITY);
	for (size_t i = 1; i < n - 1; i++) heap.PushBuild(i, area[i]);
	heap.PushBuild(endItem, INFINITY);
	heap.fixPos();
	for (int32_t i = 0; i <= endItem; i++) link[i] = visFastLink{i - 1, i + 1};
	link[endItem].next = -1;

	// The head and end are never popped: they have area infinity, all other
	// areas are finite, so when they are the minimum only they are left and
	// the retain test (2 <= retain) stops first.
	size_t removed = 0;
	while (heap.size > 0) {
		const visFastEntry current = heap.h[0];
		if (current.area > threshold) break;
		if (n - removed <= retain) break;
		heap.Pop();

		const visFastLink cl = link[current.item];
		const int32_t nextItem = cl.next;
		const int32_t prevItem = cl.prev;
		const int32_t prevPrev = link[prevItem].prev;
		const int32_t nextNext = link[nextItem].next;
		link[prevItem].next = nextItem;
		link[nextItem].prev = prevItem;
		removed++;

		if (prevPrev != -1) {
			const double a = doubleTriangleArea(p[prevPrev], p[prevItem], p[nextItem]);
			heap.Update(prevItem, std::max(a, current.area));
		}
		if (nextNext != -1) {
			const double a = doubleTriangleArea(p[prevItem], p[nextItem], p[nextNext]);
			heap.Update(nextItem, std::max(a, current.area));
		}
	}

	return link;
}

static void visTrim() {
	visFastTls.trim();
	visPackTls.trim();
}

template<typename GeometryType>
GeometryType visvalingam(const GeometryType &ls, double threshold, size_t retain) {
	int status;
	const visFastLink *link = visvalingamRun(ls, threshold, retain, status);
	if (status == 2) return visvalingamReference(ls, threshold, retain);
	if (status == 0) return ls;
	size_t m = 0;
	for (int32_t item = 0; item != -1; item = link[item].next) m++;
	GeometryType output;
	output.reserve(m);
	for (int32_t item = 0; item != -1; item = link[item].next) output.emplace_back(ls[item]);
	visTrim();
	return output;
}

// As visvalingam(), on ls itself: the remaining points are moved to the front
// (they keep their order, so each one moves to an index <= its own).
template<typename GeometryType>
static void visvalingamInPlace(GeometryType &ls, double threshold, size_t retain) {
	int status;
	const visFastLink *link = visvalingamRun(ls, threshold, retain, status);
	if (status == 2) { ls = visvalingamReference(ls, threshold, retain); return; }
	if (status == 0) return;
	size_t m = 0;
	for (int32_t item = 0; item != -1; item = link[item].next) ls[m++] = ls[item];
	ls.resize(m);
	visTrim();
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
	MultiPolygon output = mp;
	simplifyVisInPlace(output, max_distance, allValid);
	return output;
}
// The same as mp = simplifyVis(mp, max_distance, allValid), without new rings
void simplifyVisInPlace(MultiPolygon &mp, double max_distance, bool &allValid) {
	const double threshold = max_distance * max_distance * 4;
	for (auto &p : mp) {
		visvalingamInPlace(p.outer(), threshold, 4);
		for (auto &ring : p.inners()) visvalingamInPlace(ring, threshold, 4);
	}
	// Per-ring simplification can leave the multipolygon invalid. Use the
	// area-preserving repair instead of a bare make_valid: on large/complex
	// geometries an unconditional dissolve can collapse the polygon and drop
	// almost all of its area (which showed up as missing lake tiles at low
	// zoom). repair_multi_polygon keeps the simplified geometry untouched if a
	// repair would not preserve the covered area.
	allValid = repair_multi_polygon(mp);
}
