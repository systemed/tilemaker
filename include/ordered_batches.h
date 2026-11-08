/*! \file */
#ifndef _ORDERED_BATCHES_H
#define _ORDERED_BATCHES_H

#include <condition_variable>
#include <deque>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

// Writes batches of output in the order they were handed out, not the order threads
// finish them, so the output file doesn't depend on thread scheduling.
//
// A thread collects a batch's items between begin() and end(). end() writes the batch
// if it's the next one, followed by any batches that were waiting for it; otherwise it
// holds the batch. Batches must be numbered 0, 1, 2, ... in the order they're handed out,
// and a thread calls waitForRoom() before it takes its next batch: that keeps what is
// held below the limit set with setMaxHeldBytes(). The batch everyone waits for is always
// being worked on by a thread that isn't waiting, so waitForRoom() always returns.
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
		: writeItem(writeItem), itemBytes(itemBytes), flushItem(flushItem) { }

	void setMaxHeldBytes(uint64_t bytes) {
		std::lock_guard<std::mutex> lock(m);
		maxHeldBytes = bytes;
	}

	bool inBatch() const { return current().active; }

	void begin(uint64_t seq) {
		Current& c = current();
		c.active = true;
		c.seq = seq;
		c.items.clear();
	}

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
		held.emplace(c.seq, std::make_pair(bytes, std::move(c.items)));
		c.items = std::vector<T>();
		if (draining) {
			flushQueued(lock);
			return;
		}
		draining = true;
		while (!held.empty() && held.begin()->first == nextSeq) {
			auto batch = std::move(held.begin()->second);
			held.erase(held.begin());
			lock.unlock();
			{
				std::lock_guard<std::mutex> writeLock(w);
				for (auto& item : batch.second) writeItem(item);
			}
			lock.lock();
			nextSeq++;
			if (flushItem) {
				toFlush.emplace_back(std::move(batch));
				heldShrunk.notify_one();	// a waiting thread can flush it
				continue;
			}
			heldBytes -= batch.first;
			// Wake the waiting threads only once there is room. Woken for every batch
			// written, they all found no room and went back to sleep, again and again:
			// most of the system time of a 96-thread planet run.
			if (heldBytes <= maxHeldBytes) heldShrunk.notify_all();
		}
		draining = false;
		flushQueued(lock);
	}

	void waitForRoom() {
		std::unique_lock<std::mutex> lock(m);
		while (heldBytes > maxHeldBytes) {
			if (!toFlush.empty() && !flushing) flushQueued(lock);
			else heldShrunk.wait(lock);
		}
	}

	bool allWritten() {
		std::lock_guard<std::mutex> lock(m);
		return held.empty() && toFlush.empty() && !flushing;
	}

	// Held while items are written; lock it to write anything outside a batch
	std::mutex& writeMutex() { return w; }

private:
	struct Current {
		bool active = false;
		uint64_t seq = 0;
		std::vector<T> items;
	};
	// Flush the batches waiting for flushItem, unless another thread already does; called and
	// returns with m held
	void flushQueued(std::unique_lock<std::mutex> &lock) {
		if (!flushItem || flushing) return;
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

	// The batch this thread is working on (one per thread and item type)
	static Current& current() {
		static thread_local Current c;
		return c;
	}

	std::function<void(T&)> writeItem;
	std::function<uint64_t(const T&)> itemBytes;
	std::function<void(T&)> flushItem;
	uint64_t maxHeldBytes = 1ull << 30;
	std::mutex m;	// guards the fields below
	std::mutex w;	// held while writing
	std::condition_variable heldShrunk;
	bool draining = false;
	uint64_t nextSeq = 0;
	uint64_t heldBytes = 0;
	std::map<uint64_t, std::pair<uint64_t, std::vector<T>>> held;	// seq -> (bytes, items)
	std::deque<std::pair<uint64_t, std::vector<T>>> toFlush;	// written in order, waiting for flushItem
	bool flushing = false;	// a thread is flushing
};

#endif //_ORDERED_BATCHES_H
