#ifndef _PBF_READER_H
#define _PBF_READER_H

#include <istream>
#include <protozero/data_view.hpp>
#include <protozero/pbf_message.hpp>
#include <protozero/types.hpp>
#include <set>
#include <vector>

namespace PbfReader {
	namespace Schema {
	// See https://wiki.openstreetmap.org/wiki/PBF_Format#Definition_of_the_OSMHeader_fileblock
	// for more background on the PBF schema.
		enum class BlobHeader : protozero::pbf_tag_type {
			required_string_type = 1,
			optional_bytes_indexdata = 2,
			required_int32_datasize = 3
		};

		enum class Blob : protozero::pbf_tag_type {
			optional_int32_raw_size = 2, // When compressed, the uncompressed size
			oneof_data_bytes_raw = 1, // No compression
			oneof_data_bytes_zlib_data = 3,
			oneof_data_bytes_lzma_data = 4,
			// Formerly used for bzip2 compressed data. Deprecated in 2010.
			// bytes OBSOLETE_bzip2_data = 5 [deprecated=true]; // Don't reuse this tag number.
			oneof_data_bytes_lz4_data = 6,
			oneof_data_bytes_zstd_data = 7,
		};

		enum class HeaderBBox : protozero::pbf_tag_type {
			// These units are always in nanodegrees, they don't obey granularity rules.
			required_sint64_left = 1,
			required_sint64_right = 2,
			required_sint64_top = 3,
			required_sint64_bottom = 4
		};

		enum class HeaderBlock : protozero::pbf_tag_type {
			optional_HeaderBBox_bbox = 1,
			repeated_string_optional_features = 5
		};

		enum class StringTable : protozero::pbf_tag_type {
			repeated_bytes_s = 1
		};

		enum class PrimitiveBlock : protozero::pbf_tag_type {
			required_StringTable_stringtable = 1,
			repeated_PrimitiveGroup_primitivegroup = 2,
			optional_int32_granularity = 17,
			optional_int32_date_granularity = 18,
			optional_int64_lat_offset = 19,
			optional_int64_lon_offset = 20
		};

		enum class PrimitiveGroup : protozero::pbf_tag_type {
			repeated_Node_nodes = 1,
			optional_DenseNodes_dense = 2,
			repeated_Way_ways = 3,
			repeated_Relation_relations = 4,
			repeated_ChangeSet_changesets = 5
		};

		enum class DenseNodes : protozero::pbf_tag_type {
			repeated_sint64_id = 1,
			repeated_sint64_lat = 8,
			repeated_sint64_lon = 9,
			repeated_int32_keys_vals = 10
		};

		enum class Way : protozero::pbf_tag_type {
			required_int64_id = 1,
			repeated_uint32_keys = 2,
			repeated_uint32_vals = 3,
			repeated_sint64_refs = 8,
			repeated_sint64_lats = 9,
			repeated_sint64_lons = 10
		};

		enum class Relation : protozero::pbf_tag_type {
			required_int64_id = 1,
			repeated_uint32_keys = 2,
			repeated_uint32_vals = 3,
			repeated_int32_roles_sid = 8,
			repeated_sint64_memids = 9,
			repeated_MemberType_types = 10
		};
	}

	struct BlobHeader {
		std::string type;
		int32_t datasize;
	};

	struct HeaderBBox {
		double minLon, maxLon, minLat, maxLat;
	};

	struct HeaderBlock {
		bool hasBbox;
		HeaderBBox bbox;
		std::set<std::string> optionalFeatures;
	};

	enum class PrimitiveGroupType: char { Node = 1, DenseNodes = 2, Way = 3, Relation = 4, ChangeSet = 5};

	struct DenseNodes {
		struct Node {
			uint64_t id;
			int32_t lon;
			int32_t lat;
			uint32_t tagStart;
			uint32_t tagEnd;
		};

		struct Iterator {
			int32_t offset;
			Node node;
			DenseNodes& nodes;

			bool operator!=(Iterator& other) const;
			void operator++();
			Node& operator*();
		};

		std::vector<uint64_t> ids;
		std::vector<int32_t> lons;
		std::vector<int32_t> lats;
		std::vector<int32_t> tagStart;
		std::vector<int32_t> tagEnd;
		std::vector<int32_t> keyValues;
		Iterator begin();
		Iterator end();
		bool empty();
		void clear();
		void readDenseNodes(protozero::data_view data);
	};

	struct Way {
		uint64_t id;
		std::vector<uint32_t> keys;
		std::vector<uint32_t> vals;
		std::vector<uint64_t> refs;
		std::vector<int32_t> lats;
		std::vector<int32_t> lons;
	};

	struct Relation {
		enum MemberType: int { NODE = 0, WAY = 1, RELATION = 2 };
		uint64_t id;
		std::vector<uint32_t> keys;
		std::vector<uint32_t> vals;
		std::vector<uint64_t> memids;
		std::vector<int32_t> roles_sid;
		std::vector<int32_t> types;
	};

	class PrimitiveGroup;
	struct Ways {
		struct Iterator {
			protozero::pbf_message<Schema::PrimitiveGroup> message;
			int offset;
			Way& way;

			bool operator!=(Iterator& other) const;
			void operator++();
			PbfReader::Way& operator*();

			private:
			void readWay(protozero::data_view data);
		};

		Ways(PrimitiveGroup* pg, Way& way): pg(pg), way(way) {}
		Iterator begin();
		Iterator end();
		bool empty();

		private:
		friend PrimitiveGroup;
		PrimitiveGroup* pg;
		Way& way;
	};

	struct Relations {
		struct Iterator {
			protozero::pbf_message<Schema::PrimitiveGroup> message;
			int offset;
			Relation& relation;

			bool operator!=(Iterator& other) const;
			void operator++();
			PbfReader::Relation& operator*();

			private:
			void readRelation(protozero::data_view data);
		};


		Relations(PrimitiveGroup* pg, Relation& relation): pg(pg), relation(relation) {}
		Iterator begin();
		Iterator end();
		bool empty();

		private:
		friend PrimitiveGroup;
		PrimitiveGroup* pg;
		Relation& relation;
	};

	struct PrimitiveGroup {
		PrimitiveGroup(
			protozero::data_view data,
			DenseNodes& nodes,
			Way& way,
			Relation& relation
		);
		DenseNodes& nodes() const;
		Ways& ways() const;
		Relations& relations() const;
		PrimitiveGroupType type() const;

		int32_t translateNodeKeyValue(int32_t i) const;

		// Only meant to be called by our iterator, not by client code.
		void ensureData();
		protozero::data_view getDataView();
	private:
		protozero::data_view data;
		DenseNodes& denseNodes;
		mutable Ways internalWays;
		mutable Relations internalRelations;
		PrimitiveGroupType internalType;
		bool denseNodesInitialized;

	};

	class PbfReader;
	struct PrimitiveBlock {
		struct PrimitiveGroups {
			struct Iterator {
				int offset;
				std::vector<PrimitiveGroup>* groups;

				Iterator(): offset(0), groups(nullptr) {}
				Iterator(int offset, std::vector<PrimitiveGroup>& groups): offset(offset), groups(&groups) {}
				bool operator!=(Iterator& other) const;
				void operator++();
				PrimitiveGroup& operator*();
			};


			PrimitiveGroups(): groups(nullptr) {}
			PrimitiveGroups(std::vector<PrimitiveGroup>& groups): groups(&groups) {}
			Iterator begin();
			Iterator end();

			private:
			std::vector<PrimitiveGroup>* groups;
		};

		std::vector<protozero::data_view> stringTable;
		PrimitiveGroups& groups();

		private:
		friend PbfReader;
		std::vector<PrimitiveGroup> internalGroups;
		PrimitiveGroups groupsImpl;
	};

	// This is a little weird: we use a class only to get private storage
	// for multiple PBF readers. Due to the way we plumb the input files
	// elsewhere in the system, the readers don't own them, and are not
	// responsible for closing them.
	class PbfReader {
	public:
		BlobHeader readBlobHeader(std::istream& input);
		protozero::data_view readBlob(int32_t datasize, std::istream& input);
		HeaderBlock readHeaderBlock(protozero::data_view data);
		HeaderBBox readHeaderBBox(protozero::data_view data);
		PrimitiveBlock& readPrimitiveBlock(protozero::data_view data);
		void readStringTable(protozero::data_view data, std::vector<protozero::data_view>& stringTable);
		HeaderBlock readHeaderFromFile(std::istream& input);

	private:
		std::string blobStorage; // the blob as stored in the PBF
		std::string blobStorage2; // the blob after decompression, if needed
		PrimitiveBlock pb;
		DenseNodes denseNodes;
		Way way;
		Relation relation;
	};
}

#endif
