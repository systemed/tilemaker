#include <iostream>
#include <atomic>
#include <mutex>
#include <random>
#include <thread>
#include <vector>
#include "external/minunit.h"
#include "ordered_batches.h"

MU_TEST(test_ordered_batches_single_thread) {
	std::vector<int> written;
	OrderedBatches<int> batches([&](int& item) { written.push_back(item); }, [](const int&) { return 1; });

	// batch 1 finishes first, so it's held until batch 0 is written
	batches.begin(1);
	mu_check(batches.inBatch());
	batches.add(10);
	batches.add(11);
	batches.end();
	mu_check(!batches.inBatch());
	mu_check(written.empty());
	mu_check(!batches.allWritten());

	batches.begin(0);
	batches.add(0);
	batches.end();
	mu_check(written == std::vector<int>({ 0, 10, 11 }));
	mu_check(batches.allWritten());

	// an empty batch still moves the order on
	batches.begin(3);
	batches.add(30);
	batches.end();
	batches.begin(2);
	batches.end();
	mu_check(written == std::vector<int>({ 0, 10, 11, 30 }));
	mu_check(batches.allWritten());
}

MU_TEST(test_ordered_batches_threads) {
	// Batches are handed out in order and finish in any order; with a small limit on what
	// may be held, threads also have to wait for room
	const uint64_t numBatches = 2000, itemsPerBatch = 5;
	std::vector<uint64_t> written;
	OrderedBatches<uint64_t> batches([&](uint64_t& item) { written.push_back(item); }, [](const uint64_t&) { return 1; });
	batches.setMaxHeldBytes(20);

	std::mutex handOut;
	uint64_t nextSeq = 0;
	std::vector<std::thread> threads;
	for (int t = 0; t < 8; t++) {
		threads.emplace_back([&, t]() {
			std::mt19937 rng(t);
			while (true) {
				batches.waitForRoom();
				uint64_t seq;
				{
					std::lock_guard<std::mutex> lock(handOut);
					if (nextSeq == numBatches) return;
					seq = nextSeq++;
				}
				batches.begin(seq);
				for (uint64_t i = 0; i < itemsPerBatch; i++) {
					if (rng() % 4 == 0) std::this_thread::yield();
					batches.add(seq * itemsPerBatch + i);
				}
				batches.end();
			}
		});
	}
	for (auto& thread : threads) thread.join();

	mu_check(batches.allWritten());
	mu_check(written.size() == numBatches * itemsPerBatch);
	bool inOrder = true;
	for (uint64_t i = 0; i < written.size(); i++) if (written[i] != i) inOrder = false;
	mu_check(inOrder);
}

MU_TEST(test_ordered_batches_flush) {
	// With flushItem: every item is written and then flushed, both in order, flushes never
	// overlap, and a flush can be done by a thread other than the one that wrote the batch
	const uint64_t numBatches = 2000, itemsPerBatch = 5;
	std::vector<uint64_t> written, flushed;
	std::atomic<int> flushingNow(0);
	std::atomic<bool> overlap(false), flushedBeforeWritten(false);
	std::atomic<uint64_t> writtenCount(0);
	OrderedBatches<uint64_t> batches(
		[&](uint64_t& item) { written.push_back(item); writtenCount++; },
		[](const uint64_t&) { return 1; },
		[&](uint64_t& item) {
			if (flushingNow++ != 0) overlap = true;
			if (item >= writtenCount) flushedBeforeWritten = true;
			flushed.push_back(item);
			if (item % 7 == 0) std::this_thread::yield();
			flushingNow--;
		});
	batches.setMaxHeldBytes(20);

	std::mutex handOut;
	uint64_t nextSeq = 0;
	std::vector<std::thread> threads;
	for (int t = 0; t < 8; t++) {
		threads.emplace_back([&, t]() {
			std::mt19937 rng(t);
			while (true) {
				batches.waitForRoom();
				uint64_t seq;
				{
					std::lock_guard<std::mutex> lock(handOut);
					if (nextSeq == numBatches) return;
					seq = nextSeq++;
				}
				batches.begin(seq);
				for (uint64_t i = 0; i < itemsPerBatch; i++) {
					if (rng() % 4 == 0) std::this_thread::yield();
					batches.add(seq * itemsPerBatch + i);
				}
				batches.end();
			}
		});
	}
	for (auto& thread : threads) thread.join();

	mu_check(batches.allWritten());
	mu_check(!overlap);
	mu_check(!flushedBeforeWritten);
	mu_check(written.size() == numBatches * itemsPerBatch);
	mu_check(flushed.size() == numBatches * itemsPerBatch);
	bool inOrder = true;
	for (uint64_t i = 0; i < flushed.size(); i++) if (flushed[i] != i || written[i] != i) inOrder = false;
	mu_check(inOrder);
}

MU_TEST_SUITE(test_suite_ordered_batches) {
	MU_RUN_TEST(test_ordered_batches_single_thread);
	MU_RUN_TEST(test_ordered_batches_threads);
	MU_RUN_TEST(test_ordered_batches_flush);
}

int main() {
	MU_RUN_SUITE(test_suite_ordered_batches);
	MU_REPORT();
	return MU_EXIT_CODE;
}
