#include <iostream>
#include <fstream>
#include <sstream>
#include "external/minunit.h"
#include "pbf_reader.h"

MU_TEST(test_pbf_reader) {
	std::string filename;
	filename = "test/monaco.pbf";
//	filename = "/home/cldellow/Downloads/north-america-latest.osm.pbf";
//	filename = "/home/cldellow/Downloads/great-britain-latest.osm.pbf";
//	filename = "/home/cldellow/Downloads/nova-scotia-latest.osm.pbf";
	std::ifstream monaco(filename, std::ifstream::in);

	PbfReader::PbfReader reader;
	PbfReader::BlobHeader bh = reader.readBlobHeader(monaco);
	protozero::data_view blob = reader.readBlob(bh.datasize, monaco);
	PbfReader::HeaderBlock header = reader.readHeaderBlock(blob);

	mu_check(header.hasBbox);
	mu_check(header.optionalFeatures.size() == 1);
	mu_check(header.optionalFeatures.find("Sort.Type_then_ID") != header.optionalFeatures.end());

	mu_check(header.bbox.minLon == 7.409205);
	mu_check(header.bbox.maxLon == 7.448637);
	mu_check(header.bbox.minLat == 43.723350);
	mu_check(header.bbox.maxLat == 43.751690);


	bool foundNode = false, foundWay = false, foundRelation = false;
	int blocks = 0, groups = 0, strings = 0, nodes = 0, ways = 0, relations = 0;
	while (!monaco.eof()) {
		bh = reader.readBlobHeader(monaco);
		if (bh.type == "eof")
			break;


		blocks++;
		blob = reader.readBlob(bh.datasize, monaco);

		PbfReader::PrimitiveBlock pb = reader.readPrimitiveBlock(blob);

		for (const auto str : pb.stringTable) {
			if (strings == 200) {
				std::string s(str.data(), str.size());
				mu_check(s == "description:FR");
			}
			strings++;
		}

		for (const auto& group : pb.groups()) {
			groups++;
			for (const auto& node : group.nodes()) {
				nodes++;

				if (node.id == 21911886) {
					foundNode = true;

					bool foundHighwayCrossing = false;

					for (int i = node.tagStart; i < node.tagEnd; i += 2) {
						const auto keyIndex = group.translateNodeKeyValue(i);
						const auto valueIndex = group.translateNodeKeyValue(i + 1);
						std::string key(pb.stringTable[keyIndex].data(), pb.stringTable[keyIndex].size());
						std::string value(pb.stringTable[valueIndex].data(), pb.stringTable[valueIndex].size());

						if (key == "highway" && value == "crossing")
							foundHighwayCrossing = true;
					}
					mu_check(foundHighwayCrossing);
				}
			}

			for (const auto& way : group.ways()) {
				ways++;

				if (way.id == 4224978) {
					foundWay = true;

					bool foundSportSoccer = false;
					for (int i = 0; i < way.keys.size(); i++) {
						std::string key(pb.stringTable[way.keys[i]].data(), pb.stringTable[way.keys[i]].size());
						std::string value(pb.stringTable[way.vals[i]].data(), pb.stringTable[way.vals[i]].size());

						if (key == "sport" && value == "soccer")
							foundSportSoccer = true;
					}
					mu_check(foundSportSoccer);

					mu_check(way.refs.size() == 5);
					mu_check(way.refs[0] == 25178088);
					mu_check(way.refs[2] == 25178045);
					mu_check(way.refs[4] == 25178088);
				}
			}

			for (const auto& relation : group.relations()) {
				relations++;

				if (relation.id == 1124039) {
					foundRelation = true;
					mu_check(relation.memids.size() == 17);
					mu_check(relation.types.size() == 17);
					mu_check(relation.roles_sid.size() == 17);
					mu_check(relation.types[0] == PbfReader::Relation::MemberType::NODE);
					mu_check(relation.types[2] == PbfReader::Relation::MemberType::WAY);
					mu_check(relation.types[16] == PbfReader::Relation::MemberType::RELATION);
				}
			}
		}
	}

	//std::cout << blocks << " blocks, " << groups << " groups, " << nodes << " nodes, " << ways << " ways, " << relations << " relations" << std::endl;

	mu_check(foundNode);
	mu_check(foundWay);
	mu_check(foundRelation);

	mu_check(blocks == 6);
	mu_check(groups == 6);
	mu_check(strings == 8236);
	mu_check(nodes == 30477);
	mu_check(ways == 4825);
	mu_check(relations == 285);
}

MU_TEST_SUITE(test_suite_pbf_reader) {
	MU_RUN_TEST(test_pbf_reader);
}

int main() {
	MU_RUN_SUITE(test_suite_pbf_reader);
	MU_REPORT();
	return MU_EXIT_CODE;
}
