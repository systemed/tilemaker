#include <algorithm>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>

#include "pmtiles.h"
#include "helpers.h"
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <tuple>
#ifdef __linux__
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

// Tile data is written in 32 MB buffers with O_DIRECT by a thread of its own: the ordered write step
// only copies the tiles into the current buffer. One thread pushing the planet's 76 GB through ext4's
// page cache managed 1.2 GB/s (all of it kernel time) and held up the end of the tile phase; direct
// writes reach 2.1 GB/s. The unaligned rest at the end goes through the stream as before, so the file
// is the same. Where O_DIRECT isn't available (e.g. tmpfs), or with TM_NO_DIRECT set, everything goes
// through the stream.
struct PMTiles::DirectWriter {
	static constexpr size_t BUFFER = 32 << 20, ALIGN = 4096, BUFFERS = 64, WRITERS = 4;	// 2 GB absorbs bursts
	static constexpr size_t SPARE = 4;	// free buffers kept resident; the others' pages are given back
	int fd = -1;
	std::vector<char *> buffers;
	char *current = nullptr;
	size_t used = 0;
	uint64_t offset = 0;	// file offset of the current buffer
	std::mutex mutex;
	std::condition_variable changed;
	std::deque<char *> free;
	std::deque<std::tuple<char *, size_t, uint64_t>> queue;
	bool finished = false;
	int error = 0;
	std::vector<std::thread> threads;

	~DirectWriter() {
		{ std::lock_guard<std::mutex> lock(mutex); finished = true; }
		changed.notify_all();
		for (auto &t : threads) if (t.joinable()) t.join();
#ifdef __linux__
		if (fd >= 0) ::close(fd);
#endif
		for (char *b : buffers) std::free(b);
	}

	bool open(const std::string &filename, uint64_t start) {
#ifdef __linux__
		if (getenv("TM_NO_DIRECT")) return false;
		fd = ::open(filename.c_str(), O_WRONLY | O_DIRECT | O_CLOEXEC);
		if (fd < 0) return false;
		for (size_t i = 0; i < BUFFERS; i++) {
			void *b = nullptr;
			if (posix_memalign(&b, ALIGN, BUFFER) != 0) return false;
			buffers.push_back(static_cast<char *>(b));
		}
		current = buffers[0];
		for (size_t i = 1; i < BUFFERS; i++) free.push_back(buffers[i]);
		offset = start;
		for (size_t i = 0; i < WRITERS; i++) threads.emplace_back([this]() { run(); });
		return true;
#else
		(void)filename; (void)start;
		return false;
#endif
	}

	void run() {
#ifdef __linux__
		while (true) {
			std::tuple<char *, size_t, uint64_t> job;
			{
				std::unique_lock<std::mutex> lock(mutex);
				changed.wait(lock, [&]() { return !queue.empty() || finished; });
				if (queue.empty()) return;
				job = queue.front();
				queue.pop_front();
			}
			char *b = std::get<0>(job);
			size_t done = 0, length = std::get<1>(job);
			while (done < length) {
				{ std::lock_guard<std::mutex> lock(mutex); if (error != 0) break; }
				const ssize_t n = pwrite(fd, b + done, length - done, std::get<2>(job) + done);
				if (n < 0) { if (errno != EINTR) { std::lock_guard<std::mutex> lock(mutex); error = errno; } }
				else done += n;
			}
			bool spare;
			{
				// the buffer freed last is reused first: the others are only touched (made
				// resident) when a burst needs them, rather than all of them in turn
				std::lock_guard<std::mutex> lock(mutex);
				spare = free.size() < SPARE;
				if (spare) free.push_front(b);
			}
			if (!spare) {
				// A burst (the disk falling behind, e.g. busy with other work) made up to 2 GB of
				// buffers resident, and they stayed so until the end: give this one's pages back
				madvise(b, BUFFER, MADV_DONTNEED);
				std::lock_guard<std::mutex> lock(mutex);
				free.push_back(b);
			}
			changed.notify_all();
		}
#endif
	}

	void submit(size_t length) {
		std::unique_lock<std::mutex> lock(mutex);
		queue.emplace_back(current, length, offset);
		offset += length;
		changed.notify_all();
		changed.wait(lock, [&]() { return !free.empty(); });
		current = free.front();
		free.pop_front();
		used = 0;
	}

	void append(const char *data, size_t length) {
		while (length > 0) {
			const size_t n = std::min(length, BUFFER - used);
			std::memcpy(current + used, data, n);
			used += n; data += n; length -= n;
			if (used == BUFFER) submit(BUFFER);
		}
	}

	// Queues the aligned part of what is left; returns the unaligned rest and its file offset, for
	// the stream. The writes go on meanwhile (see wait).
	std::pair<std::string, uint64_t> finish() {
		const size_t aligned = used & ~(ALIGN - 1);
		std::string rest(current + aligned, used - aligned);
		uint64_t restOffset = offset + aligned;
		if (aligned > 0) submit(aligned);
		{
			std::lock_guard<std::mutex> lock(mutex);
			finished = true;
		}
		changed.notify_all();
		return { rest, restOffset };
	}

	// Waits for all writes
	void wait() {
		for (auto &t : threads) if (t.joinable()) t.join();
#ifdef __linux__
		if (fd >= 0) { ::close(fd); fd = -1; }
#endif
		if (error != 0) throw std::runtime_error(std::string("PMTiles: writing the tiles failed: ") + std::strerror(error) + " (TM_NO_DIRECT=1 writes without O_DIRECT)");
	}
};

TileOffset::TileOffset() { }
// Placing tiles (offset, repeats, index) and writing their bytes are two steps, each in batch order
// and one thread at a time, but run by different threads side by side: one thread doing both was
// the bottleneck of the tile phase at 96 threads.
PMTiles::PMTiles() :
	batches([this](PendingTile &tile) { writeTile(tile); },
	        [](const PendingTile &tile) { return tile.data.size(); },
	        [this](PendingTile &tile) { flushTile(tile); }) { }
// The flush thread writes through direct and outputStream, which are destroyed before batches:
// end it first (after a failed tile or write, main returns without close())
PMTiles::~PMTiles() { batches.finishFlushThread(); }

void PMTiles::open(std::string &filename) {
	std::cout << "Creating pmtiles at " << filename << std::endl;
	this->filename = filename;
	outputBuffer.resize(4 << 20);
	outputStream.rdbuf()->pubsetbuf(outputBuffer.data(), outputBuffer.size());
	outputStream.open(filename, std::ios::out | std::ios::trunc | std::ios::binary);
	// dummy header/root directory for now - we'll write it all later
	char header[HEADER_ROOT] = "PMTiles";
	outputStream.write(header, HEADER_ROOT);
	// the tile data follows, past the page cache if possible (the stream's header must be out first)
	outputStream.flush();
	direct.reset(new DirectWriter());
	if (!direct->open(filename, HEADER_ROOT)) direct.reset();
	// The tile data is copied out by a thread of its own: at the end of the tile phase, when the
	// last batches are written, it goes on while close() makes the directories
	batches.startFlushThread();
}

// Finish writing the .pmtiles file
void PMTiles::close(std::string &metadata) {
	std::cout << "\nClosing pmtiles file" << std::flush;
	// The tiles are all placed (the index is complete), but their data may still be on its way to
	// the file: the directories are made meanwhile, and the data finished before they're written
	std::unique_ptr<DirectWriter> directWrites;
	auto finishTileData = [&]() {
		batches.finishFlushThread();
		if (!batches.allWritten()) { throw std::runtime_error("PMTiles: a batch of tiles was never written - please file an issue"); }
		// The direct writes of the tile data go on while the rest is written (waited for at the end)
		directWrites = std::move(direct);
		if (directWrites) {
			// the rest of the tile data through the stream, which then goes on after it
			auto rest = directWrites->finish();
			outputStream.seekp(rest.second);
			outputStream.write(rest.first.data(), rest.first.size());
		}
	};
	// Big buffers that aren't needed any more are kept until the PMTiles object goes, rather than
	// freed one allocation at a time on the way (~0.2 s on a planet). Exiting, tilemaker releases its
	// memory on all threads anyway.
	auto freeAlongside = [this](auto &&garbage) {
		using T = typename std::decay<decltype(garbage)>::type;
		laterGarbage.push_back(std::make_shared<T>(std::move(garbage)));
	};

	// Add all tiles to directories: low zooms to the root, the rest to leaf directories of up to
	// LEAF_DIRECTORY_SIZE entries. The leaves are serialized and compressed on all cores (one after
	// the other that took ~9 s for the planet), then written in order, so the file is the same.
	std::vector<pmtiles::entryv3> rootEntries;
	std::vector<std::vector<pmtiles::entryv3>> leaves(1);
	// Big buffers that aren't needed any more are freed by a thread of their own, while close() goes on
	std::vector<std::thread> freeing;
	auto freeAside = [&freeing](auto &&garbage) {
		using T = typename std::decay<decltype(garbage)>::type;
		auto owned = std::make_shared<T>(std::move(garbage));
		freeing.emplace_back([owned]() mutable { owned.reset(); });
	};
	auto append = [&](uint64_t tileId, TileOffset offset) {
		pmtiles::entryv3 entry = pmtiles::entryv3(tileId, offset.offset, offset.length, 1); // 1=RLE
		if (tileId<FIRST_LEAF_TILE) {
			// <z6 so root directory
			appendWithRLE(rootEntries, entry);
		} else {
			appendWithRLE(leaves.back(), entry);
			if (leaves.back().size()>=LEAF_DIRECTORY_SIZE) leaves.emplace_back();
		}
	};
	if (isSparse) {
		for (auto it : sparseIndex) append(it.first, it.second);
	} else {
		// The root part as before; the rest (up to ~360M ids on a planet, one pass took 3 s) in
		// chunks on all threads, each run-length encoded on its own. The chunks' entries then go
		// through the same appending as single tiles would - a run of n tiles is n tiles appended
		// one after another - so the entries and the leaf boundaries are the same.
		const size_t blockSize = size_t(1) << DENSE_BLOCK_BITS;
		const size_t rootEnd = std::min<size_t>(denseSize, FIRST_LEAF_TILE);
		for (size_t tileId=0; tileId<rootEnd; tileId++) {
			const TileOffset *block = denseIndex[tileId >> DENSE_BLOCK_BITS].get();
			if (block && block[tileId & (blockSize - 1)].length != 0xffffff) append(tileId, block[tileId & (blockSize - 1)]);
		}
		const size_t n = denseSize > rootEnd ? denseSize - rootEnd : 0;
		const size_t numChunks = n == 0 ? 0 : std::min<size_t>(n, std::max(1u, std::thread::hardware_concurrency()) * 4);
		std::vector<std::vector<pmtiles::entryv3>> chunks(numChunks);
		std::atomic<size_t> nextChunk(0);
		auto encodeChunks = [&]() {
			for (size_t c = nextChunk++; c < numChunks; c = nextChunk++) {
				const size_t from = rootEnd + n * c / numChunks, to = rootEnd + n * (c + 1) / numChunks;
				std::vector<pmtiles::entryv3> &entries = chunks[c];
				for (size_t tileId = from; tileId < to; tileId++) {
					const TileOffset *block = denseIndex[tileId >> DENSE_BLOCK_BITS].get();
					if (!block) {	// no tile in the rest of the block
						tileId |= blockSize - 1;
						continue;
					}
					const TileOffset &o = block[tileId & (blockSize - 1)];
					if (o.length == 0xffffff) continue;
					if (!entries.empty() && entries.back().offset == o.offset &&
					    entries.back().tile_id == tileId - entries.back().run_length)
						entries.back().run_length++;
					else
						entries.emplace_back(tileId, o.offset, o.length, 1);
				}
			}
		};
		{
			std::vector<std::thread> threads;
			const size_t numThreads = std::min<size_t>(std::max<size_t>(1, numChunks), std::max(1u, std::thread::hardware_concurrency()));
			for (size_t t = 0; t < numThreads; t++) threads.emplace_back(encodeChunks);
			for (auto &t : threads) t.join();
		}
		freeAside(std::move(denseIndex));	// the chunks have all its tiles
		// The chunks' entries then go into the leaves as if their tiles were appended one by one
		// (appendWithRLE): a run going on in the next chunk extends the entry before it, and a leaf
		// ends when it is full - with one tile of the entry that filled it, the rest of that run
		// starting the next leaf. The positions come from two short passes, over the chunks and over
		// the leaves; then the leaves are filled on all threads (one after the other that took ~2 s).
		static_assert(LEAF_DIRECTORY_SIZE >= 2, "a leaf of one entry would fill with the first entry of a run going on");
		// (1) The entries as one list: chunk c's from position firstOf[c] on; its first entry belongs
		// to the one before when the run goes on (merged[c]), and those tiles are added to it (extra).
		std::vector<size_t> firstOf(numChunks + 1, 0);
		std::vector<char> merged(numChunks, 0);
		std::vector<std::pair<size_t, uint64_t>> extra;	// (position, tiles added), ascending
		{
			size_t g = 0;
			bool haveLast = false;
			uint64_t lastTile = 0, lastOffset = 0, lastRun = 0;
			for (size_t c = 0; c < numChunks; c++) {
				firstOf[c] = g;
				const std::vector<pmtiles::entryv3> &chunk = chunks[c];
				if (chunk.empty()) continue;
				size_t j = 0;
				if (haveLast && chunk[0].offset == lastOffset && chunk[0].tile_id == lastTile + lastRun) {
					merged[c] = 1;
					if (!extra.empty() && extra.back().first == g - 1) extra.back().second += chunk[0].run_length;
					else extra.emplace_back(g - 1, chunk[0].run_length);
					lastRun += chunk[0].run_length;
					j = 1;
				}
				if (j < chunk.size()) {
					const pmtiles::entryv3 &e = chunk.back();
					lastTile = e.tile_id; lastOffset = e.offset; lastRun = e.run_length; haveLast = true;
					g += chunk.size() - j;
				}
			}
			firstOf[numChunks] = g;
		}
		const size_t total = firstOf[numChunks];
		// the entry at position g, with the tiles merged into it
		auto entryAt = [&](size_t g) {
			const size_t c = std::upper_bound(firstOf.begin(), firstOf.end(), g) - firstOf.begin() - 1;
			pmtiles::entryv3 e = chunks[c][g - firstOf[c] + merged[c]];
			auto x = std::lower_bound(extra.begin(), extra.end(), std::make_pair(g, uint64_t(0)));
			if (x != extra.end() && x->first == g) e.run_length += x->second;
			return e;
		};
		// (2) The leaves: each starts with the rest of the run that filled the one before (if any),
		// then takes entries until it is full
		struct LeafPlan { size_t first, count; bool hasRest, cutLast; pmtiles::entryv3 rest; };
		std::vector<LeafPlan> plans;
		{
			size_t g = 0;
			bool hasRest = false;
			pmtiles::entryv3 rest(0, 0, 0, 0);
			while (g < total || hasRest) {
				LeafPlan plan { g, 0, hasRest, false, rest };
				const size_t room = std::max<size_t>(1, LEAF_DIRECTORY_SIZE - (hasRest ? 1 : 0));	// the rest doesn't fill a leaf by itself
				if (total - g < room) {
					plan.count = total - g;
					plans.push_back(plan);
					break;
				}
				plan.count = room;
				const pmtiles::entryv3 filler = entryAt(g + room - 1);
				hasRest = filler.run_length > 1;
				if (hasRest) {
					plan.cutLast = true;
					rest = pmtiles::entryv3(filler.tile_id + 1, filler.offset, filler.length, filler.run_length - 1);
				}
				plans.push_back(plan);
				g += room;
			}
		}
		// (3) Fill them, each in parts (a planet has a few leaves only), on all threads
		if (!plans.empty()) {
			leaves.assign(plans.size(), std::vector<pmtiles::entryv3>());
			const size_t partSize = 1 << 20;
			std::vector<std::pair<size_t, size_t>> parts;	// (leaf, first entry of the plan's part)
			for (size_t k = 0; k < plans.size(); k++) {
				leaves[k].resize(plans[k].count + (plans[k].hasRest ? 1 : 0));
				if (plans[k].hasRest) leaves[k][0] = plans[k].rest;
				for (size_t from = 0; from < plans[k].count; from += partSize) parts.emplace_back(k, from);
			}
			std::atomic<size_t> nextPart(0);
			auto fillLeaves = [&]() {
				for (size_t q = nextPart++; q < parts.size(); q = nextPart++) {
					const LeafPlan &plan = plans[parts[q].first];
					std::vector<pmtiles::entryv3> &leaf = leaves[parts[q].first];
					const size_t first = plan.first + parts[q].second, count = std::min(partSize, plan.count - parts[q].second);
					size_t pos = (plan.hasRest ? 1 : 0) + parts[q].second;
					// walk the chunks from the part's first entry on
					size_t c = std::upper_bound(firstOf.begin(), firstOf.end(), first) - firstOf.begin() - 1;
					size_t j = first - firstOf[c] + merged[c];
					auto x = std::lower_bound(extra.begin(), extra.end(), std::make_pair(first, uint64_t(0)));
					for (size_t g = first; g < first + count; g++, j++, pos++) {
						while (j >= chunks[c].size()) { c++; j = merged[c]; }
						leaf[pos] = chunks[c][j];
						if (x != extra.end() && x->first == g) { leaf[pos].run_length += x->second; ++x; }
					}
				}
			};
			std::vector<std::thread> threads;
			const size_t numThreads = std::min<size_t>(std::max<size_t>(1, parts.size()), std::max(1u, std::thread::hardware_concurrency()));
			for (size_t t = 0; t < numThreads; t++) threads.emplace_back(fillLeaves);
			for (auto &t : threads) t.join();
			for (size_t k = 0; k < plans.size(); k++)
				if (plans[k].cutLast) leaves[k].back().run_length = 1;
			for (const auto &leaf : leaves) numTileEntries += leaf.size();
		}
		freeAside(std::move(chunks));
	}
	if (leaves.back().empty()) leaves.pop_back();
	auto runOnAll = [](size_t jobs, const std::function<void(size_t)> &job) {
		std::atomic<size_t> next(0);
		std::vector<std::thread> threads;
		const size_t numThreads = std::min<size_t>(std::max<size_t>(1, jobs), std::max(1u, std::thread::hardware_concurrency()));
		for (size_t t = 0; t < numThreads; t++)
			threads.emplace_back([&]() { for (size_t i = next++; i < jobs; i = next++) job(i); });
		for (auto &t : threads) t.join();
	};
	std::vector<std::string> compressedLeaves;
	if (numTileEntries >= ROOT_ONLY) {
		// Serialized as pmtiles::serialize_directory does - the entry count, then the tile ids (as
		// differences), run lengths, lengths and offsets (0: right after the one before), each a
		// varint - but each column in parts, on all threads: one 10M-entry leaf took 0.4 s
		const size_t serialPart = 1 << 20;
		struct Piece { size_t leaf, column, from, to; std::string bytes; };
		std::vector<Piece> pieces;
		for (size_t i = 0; i < leaves.size(); i++)
			for (size_t column = 0; column < 4; column++)
				for (size_t from = 0; from < leaves[i].size() || from == 0; from += serialPart)
					pieces.push_back(Piece { i, column, from, std::min(leaves[i].size(), from + serialPart), std::string() });
		auto putVarint = [](std::string &out, uint64_t value) {
			while (value >= 0x80U) {
				out.push_back(char((value & 0x7fU) | 0x80U));
				value >>= 7U;
			}
			out.push_back(char(value));
		};
		runOnAll(pieces.size(), [&](size_t k) {
			Piece &piece = pieces[k];
			const std::vector<pmtiles::entryv3> &entries = leaves[piece.leaf];
			std::string &out = piece.bytes;
			out.reserve((piece.to - piece.from) * 3 + 16);
			for (size_t i = piece.from; i < piece.to; i++) {
				const pmtiles::entryv3 &e = entries[i];
				switch (piece.column) {
					case 0: putVarint(out, e.tile_id - (i > 0 ? entries[i - 1].tile_id : 0)); break;
					case 1: putVarint(out, e.run_length); break;
					case 2: putVarint(out, e.length); break;
					default:
						if (i > 0 && e.offset == entries[i - 1].offset + entries[i - 1].length) putVarint(out, 0);
						else putVarint(out, e.offset + 1);
				}
			}
		});
		compressedLeaves.resize(leaves.size());
		runOnAll(leaves.size(), [&](size_t i) {
			std::string data;
			size_t size = 10;
			for (const Piece &piece : pieces) if (piece.leaf == i) size += piece.bytes.size();
			data.reserve(size);
			putVarint(data, leaves[i].size());
			for (Piece &piece : pieces)
				if (piece.leaf == i) { data += piece.bytes; std::string().swap(piece.bytes); }
			compressedLeaves[i] = compress_string(data, Z_DEFAULT_COMPRESSION, true);
		});
	}
	finishTileData();
	leafStart = outputStream.tellp();
	if (numTileEntries < ROOT_ONLY) {
		for (auto const &leaf : leaves) rootEntries.insert(rootEntries.end(), leaf.begin(), leaf.end());
	} else {
		// write the leaf directories, and add references to them to the root directory
		std::vector<uint64_t> locations(leaves.size());
		uint64_t location = static_cast<uint64_t>(outputStream.tellp());
		for (size_t i = 0; i < leaves.size(); i++) {
			locations[i] = location;
			location += compressedLeaves[i].size();
			rootEntries.emplace_back(pmtiles::entryv3(leaves[i][0].tile_id, locations[i]-leafStart, compressedLeaves[i].size(), 0));
		}
		bool written = false;
#ifdef __linux__
		// side by side, through a file descriptor of their own (they're past what the stream holds)
		outputStream.flush();
		const int fd = ::open(filename.c_str(), O_WRONLY | O_CLOEXEC);
		if (fd >= 0) {
			std::atomic<bool> failed(false);
			runOnAll(leaves.size(), [&](size_t i) {
				size_t done = 0;
				const std::string &bytes = compressedLeaves[i];
				while (done < bytes.size() && !failed) {
					const ssize_t n = pwrite(fd, bytes.data() + done, bytes.size() - done, locations[i] + done);
					if (n < 0) { if (errno != EINTR) failed = true; }
					else done += n;
				}
			});
			if (::close(fd) != 0) failed = true;
			if (failed) throw std::runtime_error(std::string("PMTiles: writing the leaf directories failed: ") + std::strerror(errno));
			outputStream.seekp(location);
			written = true;
		}
#endif
		if (!written) {
			for (size_t i = 0; i < leaves.size(); i++)
				outputStream.write(compressedLeaves[i].c_str(), compressedLeaves[i].size());
		}
		freeAlongside(std::move(compressedLeaves));
	}
	uint64_t leafLength = static_cast<uint64_t>(outputStream.tellp()) - leafStart;

	// create JSON metadata
	std::string compressed = compress_string(metadata, Z_DEFAULT_COMPRESSION, true);
	uint64_t jsonStart = static_cast<uint64_t>(outputStream.tellp());
	int jsonLength = compressed.size();
	outputStream.write(compressed.c_str(), jsonLength);
	
	// write root directory
	std::string directory = pmtiles::serialize_directory(rootEntries);
	compressed = compress_string(directory, Z_DEFAULT_COMPRESSION, true);
	int rootLength = compressed.size();
	if (rootLength > (HEADER_ROOT-127)) { throw std::runtime_error(".pmtiles root directory was too large - please file an issue"); }
	outputStream.seekp(127);
	outputStream.write(compressed.c_str(), rootLength);

	// add all sizes to 127-byte header
	header.root_dir_offset = 127;
	header.root_dir_bytes = rootLength;
	header.json_metadata_offset = jsonStart;
	header.json_metadata_bytes = jsonLength;
	header.leaf_dirs_offset = leafStart;
	header.leaf_dirs_bytes = leafLength;
	header.tile_data_offset = HEADER_ROOT;
	header.tile_data_bytes = leafStart - HEADER_ROOT;
	header.addressed_tiles_count = numTilesAddressed;
	header.tile_entries_count = numTileEntries;
	header.tile_contents_count = numTilesWritten;
	header.clustered = false;
	header.internal_compression = pmtiles::COMPRESSION_GZIP;
	header.tile_compression = pmtiles::COMPRESSION_GZIP;
	header.tile_type = tileType;

	// write header
	std::string headerString = header.serialize();
	outputStream.seekp(0);
	outputStream.write(headerString.c_str(), headerString.size());

	// ...and we're done!
	outputStream.close();
	if (directWrites) directWrites->wait();
	freeAlongside(std::move(leaves));
	freeAlongside(std::move(directWrites));	// its buffers
	for (auto &t : freeing) t.join();
}

// Add an entry either to the current leaf directory, or (for lowzoom) the root directory
// Handle run-length encoding for the directory entries
void PMTiles::appendWithRLE(std::vector<pmtiles::entryv3> &entries, pmtiles::entryv3 &entry) {
	if (entries.empty() ||
		entries.back().offset != entry.offset ||
		entries.back().tile_id != (entry.tile_id-entries.back().run_length)) { 
			entries.emplace_back(entry); 
			numTileEntries++;
			return;
	}
	entries.back().run_length++;
}

// Save a tile: compress it here (compress_string is expensive), but leave tiny tiles
// raw until they're written, since most of them (e.g. sea) repeat an earlier one
void PMTiles::saveTile(int zoom, int x, int y, std::string &data) {
	PendingTile tile;
	tile.tileId = pmtiles::zxy_to_tileid(zoom,x,y);
	tile.isTiny = data.size()<TINY_LENGTH;
	tile.data = tile.isTiny ? data : compress_string(data, compressionLevel, true);
	if (batches.inBatch()) {
		batches.add(std::move(tile));
		return;
	}
	std::lock_guard<std::mutex> lock(batches.writeMutex());
	writeTile(tile);
	flushTile(tile);
}

void PMTiles::flushTile(PendingTile &tile) {
	if (!tile.data.empty()) {
		if (direct) direct->append(tile.data.data(), tile.data.size());
		else outputStream.write(tile.data.c_str(), tile.data.size());
	}
	tile.data = std::string();
}

// Write a tile to file and store it in the index
// - if the tile is tiny and has already been written, reuse that instead
void PMTiles::writeTile(PendingTile &tile) {
	TileOffset offset;
	auto cached = tile.isTiny ? tinyCache.find(tile.data) : tinyCache.end();
	if (cached != tinyCache.end()) {
		offset = cached->second;
		tile.data = std::string();	// already in the file
	} else {
		std::string compressed = tile.isTiny ? compress_string(tile.data, compressionLevel, true) : std::move(tile.data);
		offset = TileOffset(tileDataEnd, compressed.size());
		tileDataEnd += compressed.size();
		numTilesWritten++;
		// add to cache if tiny
		if (tile.isTiny) {
			if (tinyCache.size()>TINY_MAX_SIZE) tinyCache.clear();
			tinyCache.insert({ tile.data, offset });
		}
		tile.data = std::move(compressed);	// flushTile writes it
	}

	// store in index
	uint64_t tileId = tile.tileId;
	numTilesAddressed++;
	if (isSparse) {
		sparseIndex[tileId] = offset;
	} else {
		if (tileId >= denseSize) {
			denseSize = tileId + 10000;
			denseIndex.resize(((denseSize - 1) >> DENSE_BLOCK_BITS) + 1);
		}
		std::unique_ptr<TileOffset[]> &block = denseIndex[tileId >> DENSE_BLOCK_BITS];
		if (!block) {
			block.reset(new TileOffset[size_t(1) << DENSE_BLOCK_BITS]);
			std::fill_n(block.get(), size_t(1) << DENSE_BLOCK_BITS, TileOffset(0, 0xffffff));
		}
		block[tileId & ((size_t(1) << DENSE_BLOCK_BITS) - 1)] = offset;
	}
}
