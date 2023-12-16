#include <iostream>
#include <algorithm>
#include "external/minunit.h"
#include "attribute_store.h"

MU_TEST(test_attribute_store) {
	AttributeStore store;

	mu_check(store.size() == 0);

	AttributeSet s1;
	store.addAttribute(s1, "str1", std::string("someval"), 0);
	store.addAttribute(s1, "str2", std::string("a very long string"), 0);
	store.addAttribute(s1, "bool1", false, 0);
	store.addAttribute(s1, "bool2", true, 0);
	store.addAttribute(s1, "float1", (float)42.0, 0);

	const auto s1Index = store.add(s1);

	mu_check(store.size() == 1);

	const auto s1Pairs = store.getUnsafe(s1Index);
	mu_check(s1Pairs.size() == 5);

	const auto str1 = std::find_if(s1Pairs.begin(), s1Pairs.end(), [&store](auto ap) {
			return ap->keyIndex == store.keyStore.key2index("str1");
	});
	mu_check(str1 != s1Pairs.end());
	mu_check((*str1)->hasStringValue());
	mu_check((*str1)->stringValue() == "someval");

	const auto str2 = std::find_if(s1Pairs.begin(), s1Pairs.end(), [&store](auto ap) {
			return ap->keyIndex == store.keyStore.key2index("str2");
	});
	mu_check(str2 != s1Pairs.end());
	mu_check((*str2)->hasStringValue());
	mu_check((*str2)->stringValue() == "a very long string");

	const auto bool1 = std::find_if(s1Pairs.begin(), s1Pairs.end(), [&store](auto ap) {
			return ap->keyIndex == store.keyStore.key2index("bool1");
	});
	mu_check(bool1 != s1Pairs.end());
	mu_check((*bool1)->hasBoolValue());
	mu_check((*bool1)->boolValue() == false);

	const auto bool2 = std::find_if(s1Pairs.begin(), s1Pairs.end(), [&store](auto ap) {
			return ap->keyIndex == store.keyStore.key2index("bool2");
	});
	mu_check(bool2 != s1Pairs.end());
	mu_check((*bool2)->hasBoolValue());
	mu_check((*bool2)->boolValue() == true);

	const auto float1 = std::find_if(s1Pairs.begin(), s1Pairs.end(), [&store](auto ap) {
			return ap->keyIndex == store.keyStore.key2index("float1");
	});
	mu_check(float1 != s1Pairs.end());
	mu_check((*float1)->hasFloatValue());
	mu_check((*float1)->floatValue() == 42);

}

MU_TEST_SUITE(test_suite_attribute_store) {
	MU_RUN_TEST(test_attribute_store);
}

int main() {
	MU_RUN_SUITE(test_suite_attribute_store);
	MU_REPORT();
	return MU_EXIT_CODE;
}
