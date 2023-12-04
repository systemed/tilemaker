
#include "osm_store.h"
#include <iostream>
#include <fstream>
#include <iterator>
#include <unordered_map>

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
	: mapping(filename.c_str(), boost::interprocess::read_write)
	, region(mapping, boost::interprocess::read_write)
	, buffer(boost::interprocess::create_only, reinterpret_cast<uint8_t *>(region.get_address()) + offset, region.get_size() - offset)
	, filename(filename)
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

	std::string new_filename = mmap_dir_filename + "/mmap_" + to_string(mmap_dir.files.size()) + ".dat";
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

	std::string new_filename = mmap_dir_filename + "/mmap_" + to_string(mmap_dir.files.size()) + ".dat";
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

void NodeStore::sort(unsigned int threadNum) { 
	std::lock_guard<std::mutex> lock(mutex);
	for (auto i = 0; i < NODE_SHARDS; i++) {
		boost::sort::block_indirect_sort(
			mLatpLons[i]->begin(), mLatpLons[i]->end(), 
			[](auto const &a, auto const &b) { return a.first < b.first; }, 
			threadNum);
	}
}

void WayStore::sort(unsigned int threadNum) { 
	std::lock_guard<std::mutex> lock(mutex);
	boost::sort::block_indirect_sort(
		mLatpLonLists->begin(), mLatpLonLists->end(), 
		[](auto const &a, auto const &b) { return a.first < b.first; }, 
		threadNum);
}

static inline bool isClosed(WayStore::latplon_vector_t const &way) {
	return way.begin() == way.end();
}

void OSMStore::open(std::string const &osm_store_filename)
{
	mmap_dir.open_mmap_file(osm_store_filename);
	reopen();
	mmap_shm::close();
}

void OSMStore::nodes_sort(unsigned int threadNum) 
{
	std::cout << "\nSorting nodes" << std::endl;
	if(!use_compact_nodes)
		nodes.sort(threadNum);
}

void OSMStore::ways_sort(unsigned int threadNum) { 
	std::cout << "\nSorting ways" << std::endl;
	ways.sort(threadNum); 
}

MultiPolygon OSMStore::wayListMultiPolygon(WayVec::const_iterator outerBegin, WayVec::const_iterator outerEnd, WayVec::const_iterator innerBegin, WayVec::const_iterator innerEnd) const {
	MultiPolygon mp;
	if (outerBegin == outerEnd) { return mp; } // no outers so quit

	std::vector<LatpLonDeque> outers;
	std::vector<LatpLonDeque> inners;
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
	bool onlyOneOuter = outers.size()==1;
	for (auto ot = outers.begin(); ot != outers.end(); ot++) {
		Polygon poly;
		fillPoints(poly.outer(), ot->begin(), ot->end());
		for (auto it = filledInners.begin(); it != filledInners.end(); ++it) {
			if (onlyOneOuter || geom::within(*it, poly.outer())) { poly.inners().emplace_back(*it); }
		}
		mp.emplace_back(move(poly));
	}

	// fix winding
	geom::correct(mp);
	return mp;
}

MultiLinestring OSMStore::wayListMultiLinestring(WayVec::const_iterator outerBegin, WayVec::const_iterator outerEnd) const {
	MultiLinestring mls;
	if (outerBegin == outerEnd) { return mls; }

	std::vector<LatpLonDeque> linestrings;
	std::map<WayID,bool> done;

	mergeMultiPolygonWays(linestrings, done, outerBegin, outerEnd);

	for (auto ot = linestrings.begin(); ot != linestrings.end(); ot++) {
		Linestring ls;
		fillPoints(ls, ot->begin(), ot->end());
		mls.emplace_back(move(ls));
	}

	return mls;
}

// Assemble multipolygon constituent ways
// - Any closed polygons are added as-is
// - Linestrings are joined to existing linestrings with which they share a start/end
// - If no matches can be found, then one linestring is added (to 'attract' others)
// - The process is rerun until no ways are left
// There's quite a lot of copying going on here - could potentially be addressed
void OSMStore::mergeMultiPolygonWays(std::vector<LatpLonDeque> &results, std::map<WayID,bool> &done, WayVec::const_iterator itBegin, WayVec::const_iterator itEnd) const {

	// Create maps of start/end nodes
	std::unordered_map<LatpLon, std::vector<WayID>> startNodes;
	std::unordered_map<LatpLon, std::vector<WayID>> endNodes;
	for (auto it = itBegin; it != itEnd; ++it) {
		if (done[*it]) { continue; }
		try {
			auto const &way = ways.at(*it);
			if (isClosed(way) || results.empty()) {
				// if start==end, simply add it to the set
				results.emplace_back(way.begin(), way.end());
				done[*it] = true;
			} else {
				startNodes[way.front()].emplace_back(*it);
				endNodes[way.back()].emplace_back(*it);
			}
		} catch (std::out_of_range &err) {
			if (verbose) { cerr << "Missing way in relation: " << err.what() << endl; }
			done[*it] = true;
		}
	}

	auto deleteFromWayList = [&](LatpLon n, WayID w, bool which) {
		auto &nodemap = which ? startNodes : endNodes;
		std::vector<WayID> &waylist = nodemap.find(n)->second;
		waylist.erase(std::remove(waylist.begin(), waylist.end(), w), waylist.end());
		if (waylist.empty()) { nodemap.erase(nodemap.find(n)); }
	};
	auto removeWay = [&](WayID w) {
		auto const &way = ways.at(w);
		LatpLon first = way.front();
		LatpLon last  = way.back();
		if (startNodes.find(first) != startNodes.end()) { deleteFromWayList(first, w, true ); }
		if (startNodes.find(last)  != startNodes.end()) { deleteFromWayList(last,  w, true ); }
		if (endNodes.find(first)   != endNodes.end()  ) { deleteFromWayList(first, w, false); }
		if (endNodes.find(last)    != endNodes.end()  ) { deleteFromWayList(last,  w, false); }
		done[w] = true;
	};

	// Loop through, repeatedly adding start/end nodes if we can
	int added;
	do {
		added = 0;
		for (auto rt = results.begin(); rt != results.end(); rt++) {
			bool working=true;
			do {
				LatpLon rFirst = rt->front();
				LatpLon rLast  = rt->back();
				if (rFirst==rLast) break;
				if (startNodes.find(rLast)!=startNodes.end()) {
					// append to the result
					auto match = startNodes.find(rLast)->second;
					auto nodes = ways.at(match.back());
					rt->insert(rt->end(), nodes.begin(), nodes.end());
					removeWay(match.back());
					added++;

				} else if (endNodes.find(rLast)!=endNodes.end()) {
					// append reversed to the original
					auto match = endNodes.find(rLast)->second;
					auto nodes = ways.at(match.back());
					rt->insert(rt->end(),
						std::make_reverse_iterator(nodes.end()),
						std::make_reverse_iterator(nodes.begin()));
					removeWay(match.back());
					added++;

				} else if (endNodes.find(rFirst)!=endNodes.end()) {
					// prepend to the original
					auto match = endNodes.find(rFirst)->second;
					auto nodes = ways.at(match.back());
					rt->insert(rt->begin(), nodes.begin(), nodes.end());
					removeWay(match.back());
					added++;

				} else if (startNodes.find(rFirst)!=startNodes.end()) {
					// prepend reversed to the original
					auto match = startNodes.find(rFirst)->second;
					auto nodes = ways.at(match.back());
					rt->insert(rt->begin(),
						std::make_reverse_iterator(nodes.end()),
						std::make_reverse_iterator(nodes.begin()));
					removeWay(match.back());
					added++;

				} else { working=false; }
				
			} while (working);
		}

		// If nothing was added, then 'seed' it with a remaining unallocated way
		for (int i=0; i<=1; i++) {
			if (added>0) continue;
			for (auto nt : (i==0 ? startNodes : endNodes)) {
				WayID w = nt.second.back();
				auto const &way = ways.at(w);
				results.emplace_back(way.begin(), way.end());
				added++;
				removeWay(w);
				break;
			}
		}
	} while (added>0);
};


void OSMStore::reportStoreSize(std::ostringstream &str) {
	if (mmap_dir.mmap_file_size>0) { str << "Store size " << (mmap_dir.mmap_file_size / 1000000000) << "G | "; }
}

void OSMStore::reportSize() const {
	std::cout << "Stored " << nodes.size() << " nodes, " << ways.size() << " ways, " << relations.size() << " relations" << std::endl;
}
