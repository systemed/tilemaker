
#include "osm_store.h"
#include <iostream>
#include <iterator>

#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/managed_external_buffer.hpp>
#include <boost/interprocess/file_mapping.hpp>

#include <ciso646>
#include <boost/filesystem.hpp>
#include <boost/sort/sort.hpp>

using namespace std;
namespace bg = boost::geometry;

struct mmap_file
{
	static constexpr std::size_t increase = 1024000000;  
	static constexpr std::size_t alignment = 32;

	static size_t mmap_file_size;
	static std::string mmap_dir_filename;
	static bool mmap_dir_created;
	std::string filename;

	boost::interprocess::file_mapping mapping;
	boost::interprocess::mapped_region region;
	boost::interprocess::managed_external_buffer buffer;

	mmap_file(std::string const &filename, std::size_t offset = 0);

	static void open_mmap_file(std::string const &filename, size_t file_size = increase);

	void remove();
	static void remove_all();

	static void resize_mmap_file(size_t add_size);

	static bool is_open();
	static void flush();
};

using mmap_file_ptr = std::shared_ptr<mmap_file>;

struct mmap_shm 
{
	std::vector<uint8_t> region;
	boost::interprocess::managed_external_buffer buffer;

	static size_t mmap_file_size;

	mmap_shm(size_t size);

	static void open(size_t add_size);
	static void close();
};

using mmap_shm_ptr = std::shared_ptr<mmap_shm>;

using allocator_t = boost::interprocess::allocator<uint8_t, boost::interprocess::managed_external_buffer::segment_manager>;

std::vector<mmap_file_ptr> mmap_files;
thread_local mmap_file_ptr mmap_file_thread_ptr;

size_t mmap_file::mmap_file_size = 0;
std::string mmap_file::mmap_dir_filename;
bool mmap_file::mmap_dir_created = false;
     
std::vector< mmap_shm_ptr > mmap_shm_regions;  
size_t mmap_shm::mmap_file_size = 0;
thread_local mmap_shm_ptr mmap_shm_thread_region_ptr;

std::mutex mmap_allocator_mutex;

mmap_file::mmap_file(std::string const &filename, std::size_t offset)
	: mapping(filename.c_str(), boost::interprocess::read_write)
	, region(mapping, boost::interprocess::read_write)
	, buffer(boost::interprocess::create_only, reinterpret_cast<uint8_t *>(region.get_address()) + offset, region.get_size() - offset)
	, filename(filename)
{ }

void mmap_file::open_mmap_file(std::string const &dir_filename, size_t file_size)
{
	mmap_dir_filename = dir_filename;
	mmap_file_size = file_size;

	mmap_dir_created |= boost::filesystem::create_directory(dir_filename);

	std::string new_filename = mmap_dir_filename + "/mmap_" + to_string(mmap_files.size()) + ".dat";
	std::cout << "Filename: " << new_filename << ", size: " << mmap_file_size << std::endl;
	if(boost::filesystem::ofstream(new_filename.c_str()).fail())
		throw std::runtime_error("Failed to open mmap file");
	boost::filesystem::resize_file(new_filename.c_str(), 0);
	boost::filesystem::resize_file(new_filename.c_str(), mmap_file_size);
	mmap_file_thread_ptr = std::make_shared<mmap_file>(new_filename.c_str());

	mmap_files.emplace_back(mmap_file_thread_ptr);
}

void mmap_file::remove()
{
	if(!mmap_dir_filename.empty()) {
		try {
			boost::filesystem::remove(filename.c_str());
		} catch(boost::filesystem::filesystem_error &e) {
			std::cout << e.what() << std::endl;
		}
	}
}

void mmap_file::remove_all()
{
	if(!mmap_dir_filename.empty()) {
		try {
			for(auto &i: mmap_files) {
				i->remove();
			}

			if(mmap_dir_created) {
				boost::filesystem::remove(mmap_dir_filename.c_str());
			}
		} catch(boost::filesystem::filesystem_error &e) {
			std::cout << e.what() << std::endl;
		}
	}
}

void mmap_file::resize_mmap_file(size_t add_size)
{
	auto offset = mmap_file_size;
	auto size = increase + (add_size + alignment) - (add_size % alignment);

	std::string new_filename = mmap_dir_filename + "/mmap_" + to_string(mmap_files.size()) + ".dat";
	if(boost::filesystem::ofstream(new_filename.c_str()).fail())
		throw std::runtime_error("Failed to open mmap file");
	boost::filesystem::resize_file(new_filename.c_str(), size);
	mmap_file_thread_ptr = std::make_shared<mmap_file>(new_filename.c_str(), 0);

	mmap_files.emplace_back(mmap_file_thread_ptr);
	mmap_file_size = offset + size;
}

bool mmap_file::is_open() 
{ 
	return !mmap_files.empty(); 
}

void mmap_file::flush() 
{
	for(auto const &i: mmap_files) {
		i->region.flush();
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

void * void_mmap_allocator::allocate(size_type n, const void *hint)
{
	while(true) {
		try {
			if(mmap_file::is_open() && mmap_file_thread_ptr != nullptr) {	
				auto &i = *mmap_file_thread_ptr;
				allocator_t allocator(i.buffer.get_segment_manager());
				return &(*allocator.allocate(n, hint));
			} else if(mmap_shm_thread_region_ptr != nullptr) {	
				auto &i = *mmap_shm_thread_region_ptr;
				allocator_t allocator(i.buffer.get_segment_manager());
				return &(*allocator.allocate(n, hint));
			}
		} catch(boost::interprocess::bad_alloc &e) {
			// This mmap file is full
		}

		std::lock_guard<std::mutex> lock(mmap_allocator_mutex);
		if(mmap_file::is_open()) 
			mmap_file::resize_mmap_file(n);
		else
			mmap_shm::open(n);
	}
}

void void_mmap_allocator::deallocate(void *p, size_type n)
{
	if(mmap_shm_thread_region_ptr != nullptr) {	
		auto &i = *mmap_shm_thread_region_ptr;
		if(p >= (void const *)i.region.data()  && p < reinterpret_cast<void const *>(reinterpret_cast<uint8_t const *>(i.region.data()) + i.region.size())) {
			allocator_t allocator(i.buffer.get_segment_manager());
			return allocator.deallocate(reinterpret_cast<uint8_t *>(p), n);
		}
	}

	if(mmap_file_thread_ptr != nullptr) {	
		auto &i = *mmap_file_thread_ptr;
		if(p >= i.region.get_address()  && p < reinterpret_cast<void const *>(reinterpret_cast<uint8_t const *>(i.region.get_address()) + i.region.get_size())) {
			allocator_t allocator(i.buffer.get_segment_manager());
			return allocator.deallocate(reinterpret_cast<uint8_t *>(p), n);
		}
	} 

	//std::lock_guard<std::mutex> lock(mmap_allocator_mutex);
	/* for(auto &i: boost::adaptors::reverse(mmap_shm_regions)) {
		if(p >= (void const *)i->region.data()  && p < reinterpret_cast<void const *>(reinterpret_cast<uint8_t const *>(i->region.data()) + i->region.size())) {
			allocator_t allocator(i->buffer.get_segment_manager());
			return allocator.deallocate(reinterpret_cast<uint8_t *>(p), n);
		}
	} */
}

void void_mmap_allocator::destroy(void *p)
{
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

	//std::lock_guard<std::mutex> lock(mmap_allocator_mutex);
	/* for(auto &i: boost::adaptors::reverse(mmap_shm_regions)) {
		if(p >= (void const *)i->region.data()  && p < reinterpret_cast<void const *>(reinterpret_cast<uint8_t const *>(i->region.data()) + i->region.size())) {
			allocator_t allocator(i->buffer.get_segment_manager());
			allocator.destroy(reinterpret_cast<uint8_t *>(p));
			return;
		}
	} */
}

void NodeStore::sort(unsigned int threadNum) { 
	std::lock_guard<std::mutex> lock(mutex);
	boost::sort::block_indirect_sort(
		mLatpLons->begin(), mLatpLons->end(), 
		[](auto const &a, auto const &b) { return a.first < b.first; }, 
		threadNum);
}

void WayStore::sort(unsigned int threadNum) { 
	std::lock_guard<std::mutex> lock(mutex);
	boost::sort::block_indirect_sort(
		mNodeLists->begin(), mNodeLists->end(), 
		[](auto const &a, auto const &b) { return a.first < b.first; }, 
		threadNum);
}

static inline bool isClosed(WayStore::nodeid_vector_t const &way) {
	return way.begin() == way.end();
}

OSMStore::~OSMStore()
{
	mmap_file::remove_all();
}

void OSMStore::open(std::string const &osm_store_filename)
{
	mmap_file::open_mmap_file(osm_store_filename);
	reopen();
	mmap_shm::close();
}

MultiPolygon OSMStore::wayListMultiPolygon(WayVec::const_iterator outerBegin, WayVec::const_iterator outerEnd, WayVec::const_iterator innerBegin, WayVec::const_iterator innerEnd) const {
	MultiPolygon mp;
	if (outerBegin == outerEnd) { return mp; } // no outers so quit

	// Assemble outers
	// - Any closed polygons are added as-is
	// - Linestrings are joined to existing linestrings with which they share a start/end
	// - If no matches can be found, then one linestring is added (to 'attract' others)
	// - The process is rerun until no ways are left
	// There's quite a lot of copying going on here - could potentially be addressed
	std::vector<NodeVec> outers;
	std::vector<NodeVec> inners;
	std::map<WayID,bool> done; // true=this way has already been added to outers/inners, don't reconsider

	// merge constituent ways together
	mergeMultiPolygonWays(outers, done, outerBegin, outerEnd);
	mergeMultiPolygonWays(inners, done, innerBegin, innerEnd);

	// add all inners and outers to the multipolygon
	std::vector<Ring> filledInners;
	for (auto it = inners.begin(); it != inners.end(); ++it) {
		Ring inner;
		fillPoints(inner, it->begin(), it->end());
		filledInners.emplace_back(inner);
	}
	for (auto ot = outers.begin(); ot != outers.end(); ot++) {
		Polygon poly;
		fillPoints(poly.outer(), ot->begin(), ot->end());
		for (auto it = filledInners.begin(); it != filledInners.end(); ++it) {
			if (geom::within(*it, poly.outer())) { poly.inners().emplace_back(*it); }
		}
		mp.emplace_back(move(poly));
	}

	// fix winding
	geom::correct(mp);
	return mp;
}

void OSMStore::mergeMultiPolygonWays(std::vector<NodeVec> &results, std::map<WayID,bool> &done, WayVec::const_iterator itBegin, WayVec::const_iterator itEnd) const {

	int added;
	do {
		added = 0;
		for (auto it = itBegin; it != itEnd; ++it) {
			if (done[*it]) { continue; }
			auto const &way = ways.at(*it);
			if (isClosed(way)) {
				// if start==end, simply add it to the set
				results.emplace_back(way.begin(), way.end());
				added++;
				done[*it] = true;
			} else {
				// otherwise, can we find a matching outer to append it to?
				bool joined = false;
				auto const &nodes = ways.at(*it);
				NodeID jFirst = nodes.front();
				NodeID jLast  = nodes.back();
				for (auto ot = results.begin(); ot != results.end(); ot++) {
					NodeID oFirst = ot->front();
					NodeID oLast  = ot->back();
					if (jFirst==jLast) continue; // don't join to already-closed ways
					else if (oLast==jFirst) {
						// append to the original
						ot->insert(ot->end(), nodes.begin(), nodes.end());
						joined=true; break;
					} else if (oLast==jLast) {
						// append reversed to the original
						ot->insert(ot->end(),
							std::make_reverse_iterator(nodes.end()),
							std::make_reverse_iterator(nodes.begin()));
						joined=true; break;
					} else if (jLast==oFirst) {
						// prepend to the original
						ot->insert(ot->begin(), nodes.begin(), nodes.end());
						joined=true; break;
					} else if (jFirst==oFirst) {
						ot->insert(ot->begin(),
							std::make_reverse_iterator(nodes.end()),
							std::make_reverse_iterator(nodes.begin()));
						joined=true; break;
					}
				}
				if (joined) {
					added++;
					done[*it] = true;
				}
			}
		}
		// If nothing was added, then 'seed' it with a remaining unallocated way
		if (added==0) {
			for (auto it = itBegin; it != itEnd; ++it) {
				if (done[*it]) { continue; }
				auto const &way = ways.at(*it);
				results.emplace_back(way.begin(), way.end());
				added++;
				done[*it] = true;
				break;
			}
		}
	} while (added>0);
};


void OSMStore::reportStoreSize(std::ostringstream &str) {
	if (mmap_file::mmap_file_size>0) { str << "Store size " << (mmap_file::mmap_file_size / 1000000) << " | "; }
}

void OSMStore::reportSize() const {
	std::cout << "Stored " << nodes.size() << " nodes, " << ways.size() << " ways, " << relations.size() << " relations" << std::endl;
	std::cout << "Shape points: " << shp_generated.points_store->size() << ", lines: " << shp_generated.linestring_store->size() << ", polygons: " << shp_generated.multi_polygon_store->size() << std::endl;
	std::cout << "Generated points: " << osm_generated.points_store->size() << ", lines: " << osm_generated.linestring_store->size() << ", polygons: " << osm_generated.multi_polygon_store->size() << std::endl;
}
