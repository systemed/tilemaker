#ifndef _APPEND_VECTOR_H
#define _APPEND_VECTOR_H

#include "mmap_allocator.h"
#include <vector>
#include <queue>

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
				appendVector(&appendVector), vec(vec), offset(offset) {}

			Iterator():
				appendVector(nullptr), vec(0), offset(0) {}


			bool operator<(const Iterator& other) const {
				if (vec < other.vec)
					return true;

				if (vec > other.vec)
					return false;

				return offset < other.offset;
			}

			bool operator>=(const Iterator& other) const {
				return !(*this < other);
			}

			Iterator operator-(int delta) const {
				int64_t absolute = vec * APPEND_VECTOR_SIZE + offset;
				absolute -= delta;
				return Iterator(*appendVector, absolute / APPEND_VECTOR_SIZE, absolute % APPEND_VECTOR_SIZE);
			}

			Iterator operator+(int delta) const {
				int64_t absolute = vec * APPEND_VECTOR_SIZE + offset;
				absolute += delta;
				return Iterator(*appendVector, absolute / APPEND_VECTOR_SIZE, absolute % APPEND_VECTOR_SIZE);
			}

			bool operator==(const Iterator& other) const {
				return appendVector == other.appendVector && vec == other.vec && offset == other.offset;
			}

			bool operator!=(const Iterator& other) const {
				return !(*this == other);
			}

			std::ptrdiff_t operator-(const Iterator& other) const {
				int64_t absolute = vec * APPEND_VECTOR_SIZE + offset;
				int64_t otherAbsolute = other.vec * APPEND_VECTOR_SIZE + other.offset;

				return absolute - otherAbsolute;
			}

			reference operator*() const {
				auto& vector = appendVector->vecs[vec];
				auto& el = vector[offset];
				return el;
			}

			pointer operator->() const {
				auto& vector = appendVector->vecs[vec];
				auto& el = vector[offset];
				return &el;
			}

			Iterator& operator+= (int delta) {
				int64_t absolute = vec * APPEND_VECTOR_SIZE + offset;
				absolute += delta;

				vec = absolute / APPEND_VECTOR_SIZE;
				offset = absolute % APPEND_VECTOR_SIZE;
				return *this;
			}

			Iterator& operator-= (int delta) {
				int64_t absolute = vec * APPEND_VECTOR_SIZE + offset;
				absolute -= delta;

				vec = absolute / APPEND_VECTOR_SIZE;
				offset = absolute % APPEND_VECTOR_SIZE;
				return *this;
			}

			// Prefix increment
			Iterator& operator++() {
				offset++;
				if (offset == APPEND_VECTOR_SIZE) {
					offset = 0;
					vec++;
				}
				return *this;
			}  

			// Postfix increment
			Iterator operator++(int) { Iterator tmp = *this; ++(*this); return tmp; }

			// Prefix decrement
			Iterator& operator--() {
				if (offset > 0) {
					offset--;
				} else {
					vec--;
					offset = APPEND_VECTOR_SIZE - 1;
				}

				return *this;
			}

			// Postfix decrement
			Iterator operator--(int) { Iterator tmp = *this; --(*this); return tmp; }

		private:
			mutable AppendVector<T>* appendVector;
			int32_t vec, offset;
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
