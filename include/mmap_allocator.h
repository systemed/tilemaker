#ifndef _MMAP_ALLOCATOR_H
#define _MMAP_ALLOCATOR_H

#include <cstddef>
#include <sstream>

class void_mmap_allocator
{
public:
	typedef std::size_t size_type;

	static void *allocate(size_type n, const void *hint = 0);
	static void reportStoreSize(std::ostringstream &str);
	static void openMmapFile(const std::string& mmapFilename);
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
		// This is a no-op. Most usage of tilemaker should never deallocate
		// an mmap_allocator resource. On program termination, everything gets
		// freed.
	}

	void construct(pointer p, const_reference val)
	{
		new((void *)p) T(val);        
	}
};

template<typename T1, typename T2>
static inline bool operator==(mmap_allocator<T1> &, mmap_allocator<T2> &) { return true; }
template<typename T1, typename T2>
static inline bool operator!=(mmap_allocator<T1> &, mmap_allocator<T2> &) { return false; }

#endif
