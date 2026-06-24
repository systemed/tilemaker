#include <iostream>
#include <iterator>
#include <type_traits>
#include <boost/sort/sort.hpp>
#include "external/minunit.h"
#include "append_vector.h"

using namespace AppendVectorNS;

MU_TEST(test_append_vector) {
	AppendVector<int32_t> vec;
	AppendVector<int32_t> vec2;
	mu_check(vec.size() == 0);
	mu_check(vec.begin() == vec.end());
	mu_check(vec.begin() != vec2.begin());

	for (int i = 0; i < 10000; i++) {
		vec.push_back(i);
	}
	mu_check(vec.size() == 10000);

	mu_check(vec[25] == 25);

	const AppendVector<int32_t>::Iterator& it = vec.begin();
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
	mu_check(vec.begin()[25] == 25);
	mu_check(25 + vec.begin() == vec.begin() + 25);
	mu_check((vec.begin() + 25) - vec.begin() == 25);
	mu_check(vec.end() - vec.begin() == 10000);
	mu_check(vec.begin() < vec.end());
	mu_check(vec.begin() <= vec.begin());
	mu_check(vec.end() > vec.begin());
	mu_check(vec.end() >= vec.end());

	auto postfix = vec.begin();
	mu_check(*(postfix++) == 0);
	mu_check(*postfix == 1);
	mu_check(*(postfix--) == 1);
	mu_check(*postfix == 0);

	const int32_t chunkBoundary = 8192;
	auto boundary = vec.begin();
	boundary += chunkBoundary;
	mu_check(*boundary == chunkBoundary);
	boundary -= 1;
	mu_check(*boundary == chunkBoundary - 1);

	static_assert(std::is_same<
		std::iterator_traits<AppendVector<int32_t>::Iterator>::iterator_category,
		std::random_access_iterator_tag
	>::value, "AppendVector iterator should advertise random access");

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
		[](const int32_t& a, const int32_t& toFind) {
			return a < toFind;
		}
	);

	mu_check(iter != vec.end());
	mu_check(*iter == 123);

	iter = std::lower_bound(
		vec.begin(),
		vec.end(),
		123123,
		[](const int32_t& a, const int32_t& toFind) {
			return a < toFind;
		}
	);

	mu_check(iter == vec.end());

	iter = std::lower_bound(
		vec.begin(),
		vec.end(),
		-2,
		[](const int32_t& a, const int32_t& toFind) {
			return a < toFind;
		}
	);

	mu_check(iter == vec.begin());
}

MU_TEST_SUITE(test_suite_append_vector) {
	MU_RUN_TEST(test_append_vector);
}

int main() {
	MU_RUN_SUITE(test_suite_append_vector);
	MU_REPORT();
	return MU_EXIT_CODE;
}
