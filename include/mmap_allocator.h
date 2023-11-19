#ifndef _MMAP_ALLOCATOR_H
#define _MMAP_ALLOCATOR_H

#include <cstddef>

class void_mmap_allocator
{
public:
	typedef std::size_t size_type;

	static void *allocate(size_type n, const void *hint = 0);
	static void deallocate(void *p, size_type n);
	static void destroy(void *p);
	static void shutdown();
};

template<typename T>
class mmap_allocator
{
public:
	typedef std::size_t size_type;
	typedef std::ptrdiff_t difference_type;
	typedef T *pointer;
	typedef const T *const_pointer;
	typedef const T &const_reference;
	typedef T value_type;

	template <class U>
	struct rebind
	{
		typedef mmap_allocator<U> other;
	};

	mmap_allocator() = default;

	template<typename OtherT>
	mmap_allocator(OtherT &)
	{ }

	pointer allocate(size_type n, const void *hint = 0)
	{
		return reinterpret_cast<T *>(void_mmap_allocator::allocate(n * sizeof(T), hint));
	}

	void deallocate(pointer p, size_type n)
	{
		void_mmap_allocator::deallocate(p, n);
	}

	void construct(pointer p, const_reference val)
	{
		new((void *)p) T(val);        
	}

	void destroy(pointer p) { void_mmap_allocator::destroy(p); }
};

template<typename T1, typename T2>
static inline bool operator==(mmap_allocator<T1> &, mmap_allocator<T2> &) { return true; }
template<typename T1, typename T2>
static inline bool operator!=(mmap_allocator<T1> &, mmap_allocator<T2> &) { return false; }

#endif
