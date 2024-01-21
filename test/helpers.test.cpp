#include <iostream>
#include "external/minunit.h"
#include "helpers.h"

MU_TEST(test_get_chunks) {
	{
		auto rv = getNewlineChunks("test/test.jsonl", 1);
		mu_check(rv.size() == 1);
		mu_check(rv[0].offset == 0);
		mu_check(rv[0].length == 24);
	}

	{
		auto rv = getNewlineChunks("test/test.jsonl", 2);
		mu_check(rv.size() == 2);
		mu_check(rv[0].offset == 0);
		mu_check(rv[0].length == 12);
		mu_check(rv[1].offset == 12);
		mu_check(rv[1].length == 12);
	}

	// Dividing into 3 chunks gives a lop-sided result; one of the chunks
	// consists only of whitespace. This is OK.
	{
		auto rv = getNewlineChunks("test/test.jsonl", 3);
		mu_check(rv.size() == 3);
		mu_check(rv[0].offset == 0);
		mu_check(rv[0].length == 12);
		mu_check(rv[1].offset == 12);
		mu_check(rv[1].length == 11);
		mu_check(rv[2].offset == 23);
		mu_check(rv[2].length == 1);
	}

	// Dividing into many more chunks than is possible devolves into
	// one chunk per newline.
	{
		auto rv = getNewlineChunks("test/test.jsonl", 128);
		mu_check(rv.size() == 4);
		mu_check(rv[0].offset == 0);
		mu_check(rv[0].length == 2);
		mu_check(rv[1].offset == 2);
		mu_check(rv[1].length == 10);
		mu_check(rv[2].offset == 12);
		mu_check(rv[2].length == 11);
		mu_check(rv[3].offset == 23);
		mu_check(rv[3].length == 1);
	}
}

MU_TEST_SUITE(test_suite_get_chunks) {
	MU_RUN_TEST(test_get_chunks);
}

int main() {
	MU_RUN_SUITE(test_suite_get_chunks);
	MU_REPORT();
	return MU_EXIT_CODE;
}
