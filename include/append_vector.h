#ifndef _APPEND_VECTOR_H
#define _APPEND_VECTOR_H

#include "mmap_allocator.h"
#include <vector>
#include <queue>
#include <cstdint>
#include <iterator>

// Tilemaker collects OutputObjects in a list that
// - spills to disk
// - only gets appended to
//
// Vector is great for linear access, but resizes cause expensive disk I/O to
// copy elements.
//
// Deque is great for growing without disk I/O, but it allocates in blocks of 512,
// which is inefficient for linear access.
//
// Instead, we author a limited vector-of-vectors class that allocates in bigger chunks,
// to get the best of both worlds.

#define APPEND_VECTOR_SIZE 8192
namespace AppendVectorNS {
	template <class T>
	class AppendVector {
	public:
		struct Iterator {
			using iterator_category = std::random_access_iterator_tag;
			using difference_type   = std::ptrdiff_t;
			using value_type        = T;
			using pointer           = T*;
			using reference         = T&;

			Iterator(AppendVector<T>& appendVector, uint16_t vec, uint16_t offset):
				appendVector(&appendVector), index(difference_type(vec) * APPEND_VECTOR_SIZE + offset) {}

			Iterator():
				appendVector(nullptr), index(0) {}


			bool operator<(const Iterator& other) const {
				return index < other.index;
			}

			bool operator>(const Iterator& other) const {
				return other < *this;
			}

			bool operator<=(const Iterator& other) const {
				return !(other < *this);
			}

			bool operator>=(const Iterator& other) const {
				return !(*this < other);
			}

			Iterator operator-(difference_type delta) const {
				Iterator result = *this;
				result -= delta;
				return result;
			}

			Iterator operator+(difference_type delta) const {
				Iterator result = *this;
				result += delta;
				return result;
			}

			friend Iterator operator+(difference_type delta, const Iterator& iterator) {
				return iterator + delta;
			}

			bool operator==(const Iterator& other) const {
				return appendVector == other.appendVector && index == other.index;
			}

			bool operator!=(const Iterator& other) const {
				return !(*this == other);
			}

			difference_type operator-(const Iterator& other) const {
				return index - other.index;
			}

			reference operator*() const {
				auto& vector = appendVector->vecs[index / APPEND_VECTOR_SIZE];
				auto& el = vector[index % APPEND_VECTOR_SIZE];
				return el;
			}

			reference operator[](difference_type delta) const {
				return *(*this + delta);
			}

			pointer operator->() const {
				auto& vector = appendVector->vecs[index / APPEND_VECTOR_SIZE];
				auto& el = vector[index % APPEND_VECTOR_SIZE];
				return &el;
			}

			Iterator& operator+= (difference_type delta) {
				index += delta;
				return *this;
			}

			Iterator& operator-= (difference_type delta) {
				index -= delta;
				return *this;
			}

			// Prefix increment
			Iterator& operator++() {
				index++;
				return *this;
			}  

			// Postfix increment
			Iterator operator++(int) {
				Iterator result = *this;
				++*this;
				return result;
			}

			// Prefix decrement
			Iterator& operator--() {
				index--;
				return *this;
			}

			// Postfix decrement
			Iterator operator--(int) {
				Iterator result = *this;
				--*this;
				return result;
			}

		private:
			mutable AppendVector<T>* appendVector;
			difference_type index;
		};

		AppendVector():
			count(0),
			vecs(1) {
		}

		void clear() {
			count = 0;
			vecs.clear();
			vecs.push_back(std::vector<T, mmap_allocator<T>>());
			vecs.back().reserve(APPEND_VECTOR_SIZE);
		}

		size_t size() const {
			return count;
		}

		T& operator [](int idx) {
			auto& vec = vecs[idx / APPEND_VECTOR_SIZE];
			auto& el = vec[idx % APPEND_VECTOR_SIZE];
			return el;
		}

		Iterator begin() {
			return Iterator(*this, 0, 0);
		}

		Iterator end() {
			return Iterator(*this, vecs.size() - 1, count % APPEND_VECTOR_SIZE);
		}

		void push_back(const T& el) {
			if (vecs.back().capacity() == 0)
				vecs.back().reserve(APPEND_VECTOR_SIZE);

			vecs.back().push_back(el);

			if (vecs.back().size() == vecs.back().capacity()) {
				vecs.push_back(std::vector<T, mmap_allocator<T>>());
				vecs.back().reserve(APPEND_VECTOR_SIZE);
			}

			count++;
		}

		size_t count;
		std::deque<std::vector<T, mmap_allocator<T>>> vecs;
	};
}

#undef APPEND_VECTOR_SIZE

#endif
