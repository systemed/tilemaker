#include <iostream>
#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>
#include "external/minunit.h"
#include "pooled_string.h"

MU_TEST(test_pooled_string) {
	mu_check(PooledString("").size() == 0);
	mu_check(PooledString("").toString() == "");
	mu_check(PooledString("f").size() == 1);
	mu_check(PooledString("f").toString() == "f");
	mu_check(PooledString("hi").size() == 2);
	mu_check(PooledString("f") == PooledString("f"));
	mu_check(PooledString("f") != PooledString("g"));

	mu_check(PooledString("this is more than fifteen bytes").size() == 31);
	mu_check(PooledString("this is more than fifteen bytes") != PooledString("f"));

	PooledString big("this is also a really long string");
	mu_check(big == big);
	mu_check(big.toString() == "this is also a really long string");

	PooledString big2("this is also a quite long string");
	mu_check(big != big2);
	mu_check(big.toString() != big2.toString());

	std::string shortString("short");
	protozero::data_view shortStringView = { shortString.data(), shortString.size() };
	std::string longString("this is a very long string");
	protozero::data_view longStringView = { longString.data(), longString.size() };

	PooledString stdShortString(&shortStringView);
	mu_check(stdShortString.size() == 5);
	mu_check(stdShortString.toString() == "short");

	PooledString stdLongString(&longStringView);
	mu_check(stdLongString.size() == 26);
	mu_check(stdLongString.toString() == "this is a very long string");

	// PooledStrings that are backed by std::string have the usual
	// == semantics.
	mu_check(stdShortString == PooledString("short"));
	mu_check(PooledString("short") == stdShortString);

	mu_check(stdLongString == PooledString("this is a very long string"));
	mu_check(PooledString("this is a very long string") == stdLongString);

	mu_check(stdShortString != stdLongString);
}

MU_TEST(test_pooled_string_boundaries) {
	const std::string shortBoundary(15, 'a');
	const std::string pooledBoundary(16, 'b');
	const std::string largest(65535, 'c');

	PooledString shortString(shortBoundary);
	PooledString pooledString(pooledBoundary);
	PooledString largestString(largest);

	mu_check(shortString.size() == shortBoundary.size());
	mu_check(shortString.toString() == shortBoundary);
	mu_check(pooledString.size() == pooledBoundary.size());
	mu_check(pooledString.toString() == pooledBoundary);
	mu_check(largestString.size() == largest.size());
	mu_check(largestString.toString() == largest);

	bool caughtException = false;
	try {
		PooledString tooLarge(std::string(65536, 'd'));
	} catch (const std::runtime_error&) {
		caughtException = true;
	}
	mu_check(caughtException);

	// The largest string leaves a one-byte tail. Allocating another pooled
	// string must not invalidate the preceding table.
	PooledString nextTable(std::string(16, 'e'));
	mu_check(largestString.toString() == largest);
	mu_check(nextTable.toString() == std::string(16, 'e'));
}

MU_TEST(test_pooled_string_multiple_tables) {
	std::vector<std::string> originals;
	std::vector<PooledString> pooled;
	for (int i = 0; i < 2048; i++) {
		originals.emplace_back("pooled string " + std::to_string(i) +
		                       std::string(48, char('a' + i % 26)));
		pooled.emplace_back(originals.back());
	}

	for (size_t i = 0; i < pooled.size(); i++)
		mu_check(pooled[i].toString() == originals[i]);
}

MU_TEST(test_pooled_string_threaded) {
	const int threadCount = 8;
	const int stringsPerThread = 256;
	std::atomic<bool> start(false);
	std::atomic<bool> failed(false);
	std::vector<std::thread> threads;

	for (int thread = 0; thread < threadCount; thread++) {
		threads.emplace_back([&, thread]() {
			std::vector<std::string> originals;
			std::vector<PooledString> pooled;
			for (int i = 0; i < stringsPerThread; i++)
				originals.emplace_back("thread " + std::to_string(thread) +
				                       " string " + std::to_string(i) +
				                       std::string(48, char('a' + thread)));

			while (!start.load(std::memory_order_acquire)) {}
			for (const std::string& value : originals)
				pooled.emplace_back(value);

			for (size_t i = 0; i < pooled.size(); i++) {
				if (pooled[i].toString() != originals[i])
					failed.store(true, std::memory_order_release);
			}
		});
	}

	start.store(true, std::memory_order_release);
	for (std::thread& thread : threads)
		thread.join();

	mu_check(failed.load(std::memory_order_acquire) == false);
}

MU_TEST_SUITE(test_suite_pooled_string) {
	MU_RUN_TEST(test_pooled_string);
	MU_RUN_TEST(test_pooled_string_boundaries);
	MU_RUN_TEST(test_pooled_string_multiple_tables);
	MU_RUN_TEST(test_pooled_string_threaded);
}

int main() {
	MU_RUN_SUITE(test_suite_pooled_string);
	MU_REPORT();
	return MU_EXIT_CODE;
}
