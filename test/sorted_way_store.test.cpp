#include <iostream>
#include "external/minunit.h"
#include "sorted_way_store.h"
#include "node_store.h"

class TestNodeStore : public NodeStore {
	void clear() override {}
	void reopen() override {}
	void batchStart() override {}
	void finalize(size_t threadNum) override {}
	size_t size() const override { return 1; }
	LatpLon at(NodeID id) const override {
		return { (int32_t)id, -(int32_t)id };
	}
	void insert(const std::vector<std::pair<NodeID, LatpLon>>& elements) override {}
};

void roundtripWay(const std::vector<NodeID>& way) {
	bool compress = false;

	for (int i = 0; i < 2; i++) {
		std::vector<uint8_t> output;
		uint16_t flags = SortedWayStore::encodeWay(way, output, compress);

		if (false) {
			std::cout << "input=";
			for (const auto& node : way) {
				std::cout << node << " ";
			}
			std::cout << std::endl;
			std::cout << "flags=" << flags << ", output.size()=" << output.size() << ", ";

			for (const uint8_t byte : output)
				std::cout << " " << std::to_string(byte);
			std::cout << std::endl;
		}

		const std::vector<NodeID> roundtrip = SortedWayStore::decodeWay(flags, &output[0]);

		mu_check(roundtrip.size() == way.size());
		for (int i = 0; i < way.size(); i++) {
			//std::cout << "roundtrip[" << i << "]=" << roundtrip[i] << ", way[" << i << "]=" << way[i] << std::endl;
			mu_check(roundtrip[i] == way[i]);
		}
		compress = !compress;
	}
}

MU_TEST(test_encode_way) {
	roundtripWay({ 1 });
	roundtripWay({ 1, 2 });
	roundtripWay({ 1, 2, 1 });
	roundtripWay({ 1, 2, 3, 4 });
	roundtripWay({ 4294967295, 4294967297, 8589934592, 4, 5 });
	// 11386679771 uses the full lower 32-bits, so is a good test case that
	// zigzag encoding hasn't broken anything.
	roundtripWay({ 5056880431, 538663248, 538663257, 538663260, 538663263, 11386679771, 538663266 });

	// When the high bytes are all the same, it should take
	// less space to encode.
	{
		std::vector<uint8_t> output;
		SortedWayStore::encodeWay({ 1, 2, 3, 4 }, output, false);
		const uint16_t l1 = output.size();

		SortedWayStore::encodeWay({ 1, 8589934592, 3, 4 }, output, false);
		const uint16_t l2 = output.size();

		mu_check(l1 < l2);
	}
}

MU_TEST(test_way_store) {
	TestNodeStore ns;
	SortedWayStore sws(true, ns);
	sws.batchStart();

	std::vector<std::pair<WayID, std::vector<NodeID>>> ways;
	std::vector<NodeID> shortWay;
	shortWay.push_back(123);
	ways.push_back(std::make_pair(1, shortWay));
	ways.push_back(std::make_pair(2, shortWay));
	ways.push_back(std::make_pair(513, shortWay));

	std::vector<NodeID> longWay;
	for(int i = 200; i < 300; i++)
		longWay.push_back(i);
	ways.push_back(std::make_pair(65536, longWay));
	ways.push_back(std::make_pair(131072, longWay));

	sws.insertNodes(ways);
	sws.finalize(1);

	mu_check(sws.size() == 5);

	{
		const auto& rv = sws.at(1);
		mu_check(rv.size() == 1);
		mu_check(rv[0].latp == 123);
	}

	{
		const auto& rv = sws.at(2);
		mu_check(rv.size() == 1);
		mu_check(rv[0].latp == 123);
	}

	{
		const auto& rv = sws.at(513);
		mu_check(rv.size() == 1);
		mu_check(rv[0].latp == 123);
	}

	{
		const auto& rv = sws.at(65536);
		mu_check(rv.size() == 100);
		mu_check(rv[0].latp == 200);
		mu_check(rv[99].latp == 299);
	}

	{
		const auto& rv = sws.at(131072);
		mu_check(rv.size() == 100);
		mu_check(rv[0].latp == 200);
		mu_check(rv[99].latp == 299);
	}

	// missing things should throw std::out_of_range

	bool threw = false;
	try {
		sws.at(123123123);
	} catch (std::out_of_range &e) {
		threw = true;
	} catch (...) {}
	mu_check(threw == true);

	threw = false;
	try {
		sws.at(3);
	} catch (std::out_of_range &e) {
		threw = true;
	} catch (...) {}
	mu_check(threw == true);

}

MU_TEST(test_populate_mask) {
	uint8_t mask[32];
	std::vector<uint8_t> ids;

	{
		// No ids: all 0s
		populateMask(mask, ids);
		for(int i = 0; i < 32; i++)
			mu_check(mask[i] == 0);
	}

	{
		// Every id: all 1s
		for(int i = 0; i < 256; i++)
			ids.push_back(i);
		populateMask(mask, ids);
		for(int i = 0; i < 32; i++)
			mu_check(mask[i] == 255);
	}

	{
		// Every other ID
		ids.clear();
		for (int i = 0;  i < 256; i += 2)
			ids.push_back(i);
		populateMask(mask, ids);
		for(int i = 0; i < 32; i++)
			mu_check(mask[i] == 0b01010101);
	}
}

MU_TEST_SUITE(test_suite_sorted_way_store) {
	MU_RUN_TEST(test_encode_way);
	MU_RUN_TEST(test_way_store);
}

MU_TEST_SUITE(test_suite_bitmask) {
	MU_RUN_TEST(test_populate_mask);
}

int main() {
	MU_RUN_SUITE(test_suite_sorted_way_store);
	MU_RUN_SUITE(test_suite_bitmask);
	MU_REPORT();
	return MU_EXIT_CODE;
}
