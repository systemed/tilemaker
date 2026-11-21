/*! \file */
#ifndef _ORDERED_BATCHES_H
#define _ORDERED_BATCHES_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

// Writes batches of output in the order they were handed out, not the order threads
// finish them, so the output file doesn't depend on thread scheduling.
//
// A thread collects a batch's items between begin() and end(). end() writes the batch
// if it's the next one, followed by any batches that were waiting for it; otherwise it
// holds the batch. Batches are numbered in the order they're written, from 0 on: 0, 1, 2, ...
// unless a batch names the number of the one after it (setNext),
// and a thread calls waitForRoom() before it takes its next batch: that keeps what is
// held below the limit set with setMaxHeldBytes(). Batches can be handed out ahead of their
// place, so the batch everyone waits for may not be handed out yet, and then nobody would work
// on it: setFirstUnhanded() tells the number of the first batch not handed out yet, and when
// it's the one to be written next, waitForRoom() lets one thread go on to take it.
//
// writeItem is called by one thread at a time, with writeMutex() held, in batch order.
// If flushItem is given, a batch then goes through flushItem as well: also one thread at a time
// and in batch order, but without holding up writeItem. Whichever thread is free does it, so the
// two steps run side by side. A batch counts as held until it has been flushed.
template <typename T>
class OrderedBatches {
public:
	OrderedBatches(std::function<void(T&)> writeItem, std::function<uint64_t(const T&)> itemBytes,
	               std::function<void(T&)> flushItem = nullptr)
		: writeItem(guarded(writeItem)), itemBytes(itemBytes), flushItem(guarded(flushItem)) { }

	~OrderedBatches() { finishFlushThread(); }

	// From now on, the flush step runs on a thread of its own (instead of on whichever thread is
	// free), so that it can go on after the last batch has been written: see finishFlushThread
	void startFlushThread() {
		if (!flushItem || flushThread.joinable()) return;
		flushThread = std::thread([this]() {
			std::unique_lock<std::mutex> lock(m);
			while (true) {
				flushWanted.wait(lock, [&]() { return !toFlush.empty() || stopFlushing; });
				if (toFlush.empty()) return;
				flushing = true;
				auto batch = std::move(toFlush.front());
				toFlush.pop_front();
				lock.unlock();
				for (auto& item : batch.second) flushItem(item);
				batch.second = std::vector<T>();
				lock.lock();
				flushing = false;
				heldBytes -= batch.first;
				if (heldBytes <= maxHeldBytes) heldShrunk.notify_all();
			}
		});
	}

	// Waits until the flush thread has flushed everything handed over, and ends it
	void finishFlushThread() {
		if (!flushThread.joinable()) return;
		{
			std::lock_guard<std::mutex> lock(m);
			stopFlushing = true;
		}
		flushWanted.notify_all();
		flushThread.join();
	}

	void setMaxHeldBytes(uint64_t bytes) {
		std::lock_guard<std::mutex> lock(m);
		maxHeldBytes = bytes;
	}

	// The number of the first batch in order that isn't handed out yet (UINT64_MAX: none), kept up
	// to date by whoever hands batches out. nullptr: every batch is handed out before it's waited for.
	void setFirstUnhanded(const std::atomic<uint64_t> *key) {
		std::lock_guard<std::mutex> lock(m);
		firstUnhanded = key;
	}

	bool inBatch() const { return current().active; }

	void begin(uint64_t seq) {
		Current& c = current();
		c.active = true;
		c.seq = seq;
		c.next = seq + 1;
		c.items.clear();
	}

	// The number of the batch that is written after the one begun (default: seq + 1). The numbers
	// needn't be consecutive, but each batch must name the one that follows it.
	void setNext(uint64_t next) { current().next = next; }

	void add(T&& item) { current().items.emplace_back(std::move(item)); }

	// Hand the batch over. If it's the next one to be written and no other thread is
	// writing, this thread writes it and then any held batches that follow it; meanwhile
	// other threads just hand their batches over and carry on.
	void end() {
		Current& c = current();
		c.active = false;
		uint64_t bytes = 0;
		for (const auto& item : c.items) bytes += itemBytes(item);
		std::unique_lock<std::mutex> lock(m);
		heldBytes += bytes;
		held.emplace(c.seq, std::make_pair(std::make_pair(bytes, c.next), std::move(c.items)));
		c.items = std::vector<T>();
		if (draining) {
			flushQueued(lock);
			return;
		}
		draining = true;
		while (!held.empty() && held.begin()->first == nextSeq) {
			auto entry = std::move(held.begin()->second);
			held.erase(held.begin());
			std::pair<uint64_t, std::vector<T>> batch(entry.first.first, std::move(entry.second));
			lock.unlock();
			{
				std::lock_guard<std::mutex> writeLock(w);
				for (auto& item : batch.second) writeItem(item);
			}
			lock.lock();
			nextSeq = entry.first.second;
			if (flushItem) {
				toFlush.emplace_back(std::move(batch));
				if (flushThread.joinable()) flushWanted.notify_one();
				else heldShrunk.notify_one();	// a waiting thread can flush it
				continue;
			}
			heldBytes -= batch.first;
			// Wake the waiting threads only once there is room. Woken for every batch
			// written, they all found no room and went back to sleep, again and again:
			// most of the system time of a 96-thread planet run.
			if (heldBytes <= maxHeldBytes) heldShrunk.notify_all();
		}
		draining = false;
		// The next batch to be written may not be handed out yet: wake a thread to take it
		if (heldBytes > maxHeldBytes && unhandedNext()) heldShrunk.notify_one();
		flushQueued(lock);
	}

	// Waits while more than the limit is held. Returns UINT64_MAX, or the number of the next batch
	// to be written if nobody has it yet (see setFirstUnhanded): the caller then takes that batch,
	// unless it was handed out meanwhile. Only one thread is let go on per batch number.
	uint64_t waitForRoom() {
		std::unique_lock<std::mutex> lock(m);
		while (heldBytes > maxHeldBytes) {
			if (!toFlush.empty() && !flushing && !flushThread.joinable()) flushQueued(lock);
			else if (unhandedNext()) { tookUnhanded = nextSeq; return nextSeq; }
			else heldShrunk.wait(lock);
		}
		return UINT64_MAX;
	}

	// The number of the next batch to be written: every batch before it is written
	uint64_t nextToWrite() {
		std::lock_guard<std::mutex> lock(m);
		return nextSeq;
	}

	bool allWritten() {
		std::lock_guard<std::mutex> lock(m);
		return held.empty() && toFlush.empty() && !flushing;
	}

	// A write that threw (a full disk, an SQLite error): see guarded. Once failed() the
	// items handed over are dropped, not written; error() is the first exception.
	bool failed() const { return writeFailed.load(std::memory_order_relaxed); }
	std::exception_ptr error() {
		std::lock_guard<std::mutex> lock(errorMutex);
		return firstError;
	}

	// Held while items are written; lock it to write anything outside a batch
	std::mutex& writeMutex() { return w; }

private:
	// A write that throws must not stop the batches: a thread that is writing them holds
	// the others up (draining), and an exception escaping a worker thread ends the program.
	// The first exception is kept, and from then on items are dropped instead of written,
	// so the batches still go through in order and no thread waits for one.
	std::function<void(T&)> guarded(std::function<void(T&)> write) {
		if (!write) return nullptr;
		return [this, write](T& item) {
			if (writeFailed.load(std::memory_order_relaxed)) return;
			try {
				write(item);
			} catch (...) {
				std::lock_guard<std::mutex> lock(errorMutex);
				if (!firstError) firstError = std::current_exception();
				writeFailed = true;
			}
		};
	}

	struct Current {
		bool active = false;
		uint64_t seq = 0, next = 1;
		std::vector<T> items;
	};
	// Flush the batches waiting for flushItem, unless another thread already does; called and
	// returns with m held
	void flushQueued(std::unique_lock<std::mutex> &lock) {
		if (!flushItem || flushing || flushThread.joinable()) return;
		flushing = true;
		while (!toFlush.empty()) {
			auto batch = std::move(toFlush.front());
			toFlush.pop_front();
			lock.unlock();
			for (auto& item : batch.second) flushItem(item);
			batch.second = std::vector<T>();
			lock.lock();
			heldBytes -= batch.first;
			if (heldBytes <= maxHeldBytes) heldShrunk.notify_all();
		}
		flushing = false;
		// batches queued while finishing the last one are taken by the next thread that comes by;
		// wake one in case all others wait for room
		heldShrunk.notify_one();
	}

	// The next batch to be written isn't handed out yet, and no thread was let go on to take it
	bool unhandedNext() const {
		return firstUnhanded && nextSeq >= firstUnhanded->load() && tookUnhanded != nextSeq;
	}

	// The batch this thread is working on (one per thread and item type)
	static Current& current() {
		static thread_local Current c;
		return c;
	}

	std::function<void(T&)> writeItem;
	std::function<uint64_t(const T&)> itemBytes;
	std::function<void(T&)> flushItem;
	uint64_t maxHeldBytes = 1ull << 30;
	std::mutex errorMutex;	// guards firstError
	std::exception_ptr firstError;
	std::atomic<bool> writeFailed{false};
	std::mutex m;	// guards the fields below
	std::mutex w;	// held while writing
	std::condition_variable heldShrunk;
	bool draining = false;
	uint64_t nextSeq = 0;
	uint64_t heldBytes = 0;
	std::map<uint64_t, std::pair<std::pair<uint64_t, uint64_t>, std::vector<T>>> held;	// seq -> ((bytes, next seq), items)
	std::deque<std::pair<uint64_t, std::vector<T>>> toFlush;	// written in order, waiting for flushItem
	bool flushing = false;	// a thread is flushing
	std::thread flushThread;	// see startFlushThread
	std::condition_variable flushWanted;
	bool stopFlushing = false;
	const std::atomic<uint64_t> *firstUnhanded = nullptr;
	uint64_t tookUnhanded = UINT64_MAX;	// waitForRoom() let a thread go on to take this batch
};

#endif //_ORDERED_BATCHES_H
