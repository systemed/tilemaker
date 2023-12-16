#ifndef _POOLED_STRING_H
#define _POOLED_STRING_H

// std::string allows the allocated size to differ from the used size,
// which means it needs an extra pointer. It also supports large strings.
//
// Our use case does not require this: we have immutable strings and always
// know their exact size, which fit in 64K.
//
// Further, g++'s implementation of std::string is inefficient - it takes 32
// bytes (vs clang's 24 bytes), while only allowing a small-string optimization
// for strings of length 15 or less.
//
// std::string also needs to be able to free its allocated memory -- in our case,
// we're fine with the memory living until the process dies.
//
// Instead, we implemented `PooledString`. It has a size of 16 bytes, and a small
// string optimization for strings <= 15 bytes. (We will separately teach
// AttributePair to encode Latin-character strings more efficiently, so that many
// strings of size 24 or less fit in 15 bytes.)
//
// If it needs to allocate memory, it does so from a shared pool. It is unable
// to free the memory once allocated.

#include <vector>
#include <string>

namespace PooledStringNS {
  class PooledString {
    public:
      PooledString(const std::string& str);
      size_t size() const;
      bool operator==(const PooledString& other) const;
      bool operator!=(const PooledString& other) const;
      std::string toString() const;

    private:
      // 0..3 is index into table, 4..5 is offset, 6..7 is length
      uint8_t storage[16];
  };
}

using PooledString = PooledStringNS::PooledString;

#endif
