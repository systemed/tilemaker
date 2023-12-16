#include "mmap_allocator.h"
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/managed_external_buffer.hpp>
#include <boost/interprocess/file_mapping.hpp>

#include <boost/filesystem.hpp>


struct mmap_file
{
	std::string filename;

	std::mutex mutex;
	boost::interprocess::file_mapping mapping;
	boost::interprocess::mapped_region region;
	boost::interprocess::managed_external_buffer buffer;

	mmap_file(std::string const &filename, std::size_t offset = 0);
	~mmap_file();

	void remove();
};

using mmap_file_ptr = std::shared_ptr<mmap_file>;

struct mmap_dir_t
{
	static constexpr std::size_t increase = 1024000000;  
	static constexpr std::size_t alignment = 32;

	std::string mmap_dir_filename;
	bool mmap_dir_created = false;

public:
	~mmap_dir_t();

	bool is_open();
	void open_mmap_file(std::string const &filename, size_t file_size = increase);
	void resize_mmap_file(size_t add_size);
	
	size_t mmap_file_size = 0;

	std::vector<mmap_file_ptr> files;
};

thread_local mmap_file_ptr mmap_file_thread_ptr;

struct mmap_shm 
{
	std::mutex mutex;
	std::vector<uint8_t> region;
	boost::interprocess::managed_external_buffer buffer;

	static size_t mmap_file_size;

	mmap_shm(size_t size);

	static void open(size_t add_size);
	static void close();
};

using mmap_shm_ptr = std::shared_ptr<mmap_shm>;

using allocator_t = boost::interprocess::allocator<uint8_t, boost::interprocess::managed_external_buffer::segment_manager>;

static mmap_dir_t mmap_dir;
     
std::vector< mmap_shm_ptr > mmap_shm_regions;  
size_t mmap_shm::mmap_file_size = 0;
thread_local mmap_shm_ptr mmap_shm_thread_region_ptr;

std::mutex mmap_allocator_mutex;

mmap_file::mmap_file(std::string const &filename, std::size_t offset)
	: filename(filename)
	, mapping(filename.c_str(), boost::interprocess::read_write)
	, region(mapping, boost::interprocess::read_write)
	, buffer(boost::interprocess::create_only, reinterpret_cast<uint8_t *>(region.get_address()) + offset, region.get_size() - offset)
{ }

mmap_file::~mmap_file()
{
	mapping = boost::interprocess::file_mapping();
	region = boost::interprocess::mapped_region();
	buffer = boost::interprocess::managed_external_buffer();

	remove();
}

void mmap_file::remove()
{
	if(!filename.empty()) {
		try {
			boost::filesystem::remove(filename.c_str());
		} catch(boost::filesystem::filesystem_error &e) {
			std::cout << e.what() << std::endl;
		}
	}
}

void mmap_dir_t::open_mmap_file(std::string const &dir_filename, size_t file_size)
{
	mmap_dir_filename = dir_filename;
	mmap_file_size = file_size;

	mmap_dir_created |= boost::filesystem::create_directory(dir_filename);

	std::string new_filename = mmap_dir_filename + "/mmap_" + std::to_string(mmap_dir.files.size()) + ".dat";
	std::cout << "Filename: " << new_filename << ", size: " << mmap_file_size << std::endl;
	if(std::ofstream(new_filename.c_str()).fail())
		throw std::runtime_error("Failed to open mmap file");
	boost::filesystem::resize_file(new_filename.c_str(), 0);
	boost::filesystem::resize_file(new_filename.c_str(), mmap_file_size);
	mmap_file_thread_ptr = std::make_shared<mmap_file>(new_filename.c_str());

	mmap_dir.files.emplace_back(mmap_file_thread_ptr);
}

void mmap_dir_t::resize_mmap_file(size_t add_size)
{
	auto offset = mmap_file_size;
	auto size = increase + (add_size + alignment) - (add_size % alignment);

	std::string new_filename = mmap_dir_filename + "/mmap_" + std::to_string(mmap_dir.files.size()) + ".dat";
	if(std::ofstream(new_filename.c_str()).fail())
		throw std::runtime_error("Failed to open mmap file");
	boost::filesystem::resize_file(new_filename.c_str(), size);
	mmap_file_thread_ptr = std::make_shared<mmap_file>(new_filename.c_str(), 0);

	mmap_dir.files.emplace_back(mmap_file_thread_ptr);
	mmap_file_size = offset + size;
}

bool mmap_dir_t::is_open() 
{ 
	return !mmap_dir.files.empty(); 
}

 mmap_dir_t::~mmap_dir_t()
{
	if(!mmap_dir_filename.empty()) {
		try {
			files.clear();

			if(mmap_dir_created) {
				boost::filesystem::remove(mmap_dir_filename.c_str());
			}
		} catch(boost::filesystem::filesystem_error &e) {
			std::cout << e.what() << std::endl;
		}
	}
}

mmap_shm::mmap_shm(size_t size) 
	: region(size)
	, buffer(boost::interprocess::create_only, region.data(), region.size())
{ }

void mmap_shm::open(size_t add_size)
{
	constexpr std::size_t increase = 64000000;  
	constexpr std::size_t alignment = 32;

	auto size = increase + (add_size + alignment) - (add_size % alignment);
	mmap_shm_thread_region_ptr = std::make_shared<mmap_shm>(size);
	mmap_shm_regions.emplace_back(mmap_shm_thread_region_ptr);
	mmap_file_size += size;
}

void mmap_shm::close() 
{
	mmap_shm_regions.clear();
	mmap_shm_thread_region_ptr.reset();
}

bool void_mmap_allocator_shutdown = false;

void void_mmap_allocator::shutdown() { void_mmap_allocator_shutdown = true; }

void * void_mmap_allocator::allocate(size_type n, const void *hint)
{
	while(true) {
		try {
			if(mmap_dir.is_open() && mmap_file_thread_ptr != nullptr) {	
				auto &i = *mmap_file_thread_ptr;
				std::lock_guard<std::mutex> lock(i.mutex);
				allocator_t allocator(i.buffer.get_segment_manager());
				return &(*allocator.allocate(n, hint));
			} else if(mmap_shm_thread_region_ptr != nullptr) {	
				auto &i = *mmap_shm_thread_region_ptr;
				std::lock_guard<std::mutex> lock(i.mutex);
				allocator_t allocator(i.buffer.get_segment_manager());
				return &(*allocator.allocate(n, hint));
			}
		} catch(boost::interprocess::bad_alloc &e) {
			// This mmap file is full
		}

		std::lock_guard<std::mutex> lock(mmap_allocator_mutex);
		if(mmap_dir.is_open()) 
			mmap_dir.resize_mmap_file(n);
		else
			mmap_shm::open(n);
	}
}

void void_mmap_allocator::deallocate(void *p, size_type n)
{
	destroy(p);
}

void void_mmap_allocator::destroy(void *p)
{
	if(void_mmap_allocator_shutdown) return;

	if(mmap_shm_thread_region_ptr != nullptr) {	
		auto &i = *mmap_shm_thread_region_ptr;
		if(p >= (void const *)i.region.data()  && p < reinterpret_cast<void const *>(reinterpret_cast<uint8_t const *>(i.region.data()) + i.region.size())) {
			allocator_t allocator(i.buffer.get_segment_manager());
			return allocator.destroy(reinterpret_cast<uint8_t *>(p));
		}
	}

	if(mmap_file_thread_ptr != nullptr) {	
		auto &i = *mmap_file_thread_ptr;
		if(p >= i.region.get_address()  && p < reinterpret_cast<void const *>(reinterpret_cast<uint8_t const *>(i.region.get_address()) + i.region.get_size())) {
			allocator_t allocator(i.buffer.get_segment_manager());
			allocator.destroy(reinterpret_cast<uint8_t *>(p));
			return;
		}
	} 

	std::lock_guard<std::mutex> lock(mmap_allocator_mutex);
	for(auto &i: mmap_shm_regions) {
		if(p >= (void const *)i->region.data()  && p < reinterpret_cast<void const *>(reinterpret_cast<uint8_t const *>(i->region.data()) + i->region.size())) {
			std::lock_guard<std::mutex> lock(i->mutex);
			allocator_t allocator(i->buffer.get_segment_manager());
			allocator.destroy(reinterpret_cast<uint8_t *>(p));
			return;
		}
	}

	for(auto &i: mmap_dir.files) {
		if(p >= i->region.get_address()  && p < reinterpret_cast<void const *>(reinterpret_cast<uint8_t const *>(i->region.get_address()) + i->region.get_size())) {
			std::lock_guard<std::mutex> lock(i->mutex);
			allocator_t allocator(i->buffer.get_segment_manager());
			allocator.destroy(reinterpret_cast<uint8_t *>(p));
			return;
		}
	} 
}

void void_mmap_allocator::reportStoreSize(std::ostringstream &str) {
	if (mmap_dir.mmap_file_size>0) { str << "Store size " << (mmap_dir.mmap_file_size / 1000000000) << "G | "; }
}

void void_mmap_allocator::openMmapFile(const std::string& mmapFilename) {
	mmap_dir.open_mmap_file(mmapFilename);
}



