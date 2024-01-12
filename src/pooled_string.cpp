#include "pooled_string.h"
#include <stdexcept>
#include <mutex>
#include <cstring>

namespace PooledStringNS {
	std::vector<char*> tables;
	std::mutex mutex;

	const uint8_t ShortString = 0b00;
	const uint8_t HeapString = 0b10;
	const uint8_t DataViewString = 0b11;

	// Each thread has its own string table, we only take a lock
	// to push a new table onto the vector.
	thread_local int64_t tableIndex = -1;
	thread_local int64_t spaceLeft = -1;
}

PooledString::PooledString(const std::string& str) {
	if (str.size() >= 65536)
		throw std::runtime_error("cannot store string longer than 64K");

	if (str.size() <= 15) {
		storage[0] = str.size();
		memcpy(storage + 1, str.data(), str.size());
		memset(storage + 1 + str.size(), 0, 16 - 1 - str.size());
	} else {
		memset(storage + 8, 0, 8);
		storage[0] = 1 << 7;

		if (spaceLeft < 0 || spaceLeft < str.size()) {
			std::lock_guard<std::mutex> lock(mutex);
			spaceLeft = 65536;
			char* buffer = (char*)malloc(spaceLeft);
			if (buffer == 0)
				throw std::runtime_error("PooledString could not malloc");
			tables.push_back(buffer);
			tableIndex = tables.size() - 1;
		}

		storage[1] = tableIndex >> 16;
		storage[2] = tableIndex >> 8;
		storage[3] = tableIndex;

		uint16_t offset = 65536 - spaceLeft;
		storage[4] = offset >> 8;
		storage[5] = offset;

		uint16_t length = str.size();
		storage[6] = length >> 8;
		storage[7] = length;

		memcpy(tables[tableIndex] + offset, str.data(), str.size());

		spaceLeft -= str.size();
	}
}

PooledString::PooledString(const protozero::data_view* str) {
	storage[0] = DataViewString << 6;

	*(const protozero::data_view**)((void*)(storage + 8)) = str;
}

bool PooledStringNS::PooledString::operator==(const PooledString& other) const {
	// NOTE: We have surprising equality semantics!
	//
	// If one of the strings is a DataViewString, it's value equality.
	//
	// Else, for short strings, you are equal if the strings are equal.
	//
	// For large strings, you are equal if you use the same heap memory locations.
	// This implies that someone outside of PooledString is managing pooling! In our
	// case, it is the responsibility of AttributePairStore.
	uint8_t kind = storage[0] >> 6;
	uint8_t otherKind = other.storage[0] >> 6;

	if (kind == DataViewString || otherKind == DataViewString) {
		size_t mySize = size();
		if (mySize != other.size())
			return false;

		return memcmp(data(), other.data(), mySize) == 0;
	}

	return memcmp(storage, other.storage, 16) == 0;
}

bool PooledStringNS::PooledString::operator!=(const PooledString& other) const {
	return !(*this == other);
}

const char* PooledStringNS::PooledString::data() const {
	uint8_t kind = storage[0] >> 6;

	if (kind == ShortString)
		return (char *)(storage + 1);

	if (kind == DataViewString) {
		const protozero::data_view* str = *(const protozero::data_view**)((void*)(storage + 8));
		return str->data();
	}

	uint32_t tableIndex = (storage[1] << 16) + (storage[2] << 8) + storage[3];
	uint16_t offset = (storage[4] << 8) + storage[5];

	const char* data = tables[tableIndex] + offset;
	return data;
}

size_t PooledStringNS::PooledString::size() const {
	uint8_t kind = storage[0] >> 6;
	// If the uppermost bit is set, we're in heap.
	if (kind == HeapString) {
		uint16_t length = (storage[6] << 8) + storage[7];
		return length;
	}

	if (kind == ShortString)
		// Otherwise it's stored in the lower 7 bits of the highest byte.
		return storage[0] & 0b01111111;

	const protozero::data_view* str = *(const protozero::data_view**)((void*)(storage + 8));
	return str->size();
}

std::string PooledStringNS::PooledString::toString() const {
	std::string rv;
	uint8_t kind = storage[0] >> 6;
	if (kind == HeapString) {
		// heap
		rv.reserve(size());

		uint32_t tableIndex = (storage[1] << 16) + (storage[2] << 8) + storage[3];
		uint16_t offset = (storage[4] << 8) + storage[5];

		char* data = tables[tableIndex] + offset;
		rv.append(data, size());
		return rv;
	}

	if (kind == ShortString) {
		for (int i = 0; i < storage[0]; i++)
			rv += storage[i + 1];
		return rv;
	}

	const protozero::data_view* str = *(const protozero::data_view**)((void*)(storage + 8));
	return std::string(str->data(), str->size());
}

void PooledStringNS::PooledString::ensureStringIsOwned() {
	uint8_t kind = storage[0] >> 6;

	if (kind != DataViewString)
		return;

	*this = PooledString(toString());
}

bool PooledStringNS::PooledString::operator<(const PooledString& other) const {
	size_t mySize = size();
	size_t otherSize = other.size();

	if (mySize != otherSize)
		return mySize < otherSize;

	return memcmp(data(), other.data(), mySize) < 0;
}

