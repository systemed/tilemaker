#include <iostream>
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

MU_TEST_SUITE(test_suite_pooled_string) {
	MU_RUN_TEST(test_pooled_string);
}

int main() {
	MU_RUN_SUITE(test_suite_pooled_string);
	MU_REPORT();
	return MU_EXIT_CODE;
}
