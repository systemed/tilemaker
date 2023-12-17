#include <iostream>
#include <boost/sort/sort.hpp>
#include "external/minunit.h"
#include "append_vector.h"

using namespace AppendVectorNS;

MU_TEST(test_append_vector) {
	AppendVector<uint32_t> vec;
	mu_check(vec.size() == 0);

	for (int i = 0; i < 10000; i++) {
		vec.push_back(i);
	}
	mu_check(vec.size() == 10000);

	mu_check(vec[25] == 25);

	const AppendVector<uint32_t>::Iterator& it = vec.begin();
	mu_check(*it == 0);
	mu_check(*(it + 1) == 1);
	mu_check(*(it + 2) == 2);
	mu_check(*(it + 9000) == 9000);
	mu_check(*(it + 1 - 1) == 0);
	mu_check(*(vec.end() + -1) == 9999);
	mu_check(*(vec.end() - 1) == 9999);
	mu_check(*(vec.end() - 2) == 9998);
	mu_check(*(vec.end() - 9000) == 1000);
	mu_check(*(vec.begin() - -1) == 1);

	boost::sort::block_indirect_sort(
		vec.begin(),
		vec.end(),
		[](auto const &a, auto const&b) { return b < a; },
		1
	);

	mu_check(vec[0] == 9999);
	mu_check(vec[9999] == 0);

	boost::sort::block_indirect_sort(
		vec.begin(),
		vec.end(),
		[](auto const &a, auto const&b) { return a < b; },
		1
	);

	mu_check(vec[0] == 0);
	mu_check(vec[9999] == 9999);

	auto iter = std::lower_bound(
		vec.begin(),
		vec.end(),
		123,
		[](const uint32_t& a, const uint32_t& toFind) {
			return a < toFind;
		}
	);

	mu_check(iter != vec.end());
	mu_check(*iter == 123);

	iter = std::lower_bound(
		vec.begin(),
		vec.end(),
		123123,
		[](const uint32_t& a, const uint32_t& toFind) {
			return a < toFind;
		}
	);

	mu_check(iter == vec.end());

}

MU_TEST_SUITE(test_suite_append_vector) {
	MU_RUN_TEST(test_append_vector);
}

int main() {
	MU_RUN_SUITE(test_suite_append_vector);
	MU_REPORT();
	return MU_EXIT_CODE;
}

