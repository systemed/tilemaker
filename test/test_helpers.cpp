#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/unit_test.hpp>

#include "../include/helpers.h"

BOOST_AUTO_TEST_SUITE(HelpersSuite)

BOOST_AUTO_TEST_CASE(CompressDecompress) {
    std::string original = "hello world";
    std::string compressed = compress_string(original, 9);
    BOOST_REQUIRE_EQUAL(decompress_string(compressed, false), original);
}

BOOST_AUTO_TEST_SUITE_END()
