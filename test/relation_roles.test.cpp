#include <iostream>
#include "external/minunit.h"
#include "relation_roles.h"

MU_TEST(test_relation_roles) {
	RelationRoles rr;

	mu_check(rr.getRole(0) == "");
	mu_check(rr.getOrAddRole("inner") == rr.getOrAddRole("inner"));
	mu_check(rr.getOrAddRole("never before seen") == rr.getOrAddRole("never before seen"));
	mu_check(rr.getRole(rr.getOrAddRole("inner")) == "inner");
	mu_check(rr.getRole(rr.getOrAddRole("never before seen")) == "never before seen");
}

MU_TEST_SUITE(test_suite_relation_roles) {
	MU_RUN_TEST(test_relation_roles);
}

int main() {
	MU_RUN_SUITE(test_suite_relation_roles);
	MU_REPORT();
	return MU_EXIT_CODE;
}

