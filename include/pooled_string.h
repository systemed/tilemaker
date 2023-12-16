#ifndef _POOLED_STRING_H
#define _POOLED_STRING_H

// std::string is quite general:
// - mutable
// - unlimited length
// - capacity can differ from size
// - can deallocate its dynamic memory
//
// Our use case, by contrast is immutable, bounded strings that live for the
// duration of the process.
//
// This gives us some room to have less memory overhead, especially on
// g++, whose implementation of std::string requires 32 bytes.
//
// Thus, we implement `PooledString`. It has a size of 16 bytes, and a small
// string optimization for strings <= 15 bytes. (We will separately teach
// AttributePair to encode Latin-character strings more efficiently, so that many
// strings of size 24 or less fit in 15 bytes.)
//
// If it needs to allocate memory, it does so from a shared pool. It is unable
// to free the memory once allocated.

// PooledString has one of three modes:
// - [126:127] = 00: small-string, length is in [120:125], lower 15 bytes are string
// - [126:127] = 10: pooled string, table is in bytes 1..3, offset in bytes 4..5, length in bytes 6..7
// - [126:127] = 11: pointer to std::string, pointer is in bytes 8..15
//
// Note that the pointer mode is not safe to be stored. It exists just to allow
// lookups in the AttributePair map before deciding to allocate a string.

#include <vector>
#include <string>

namespace PooledStringNS {
  class PooledString {
    public:
      // Create a short string or heap string, long-lived.
      PooledString(const std::string& str);


      // Create a std string - only valid so long as the string that is
      // pointed to is valid.
      PooledString(const std::string* str);
      size_t size() const;
      bool operator<(const PooledString& other) const;
      bool operator==(const PooledString& other) const;
      bool operator!=(const PooledString& other) const;
      std::string toString() const;
      const char* data() const;
      void ensureStringIsOwned();

    private:
      // 0..3 is index into table, 4..5 is offset, 6..7 is length
      uint8_t storage[16];
  };
}

using PooledString = PooledStringNS::PooledString;

#endif
