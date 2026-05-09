// Copyright 2011 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "util.h"

#ifdef __CYGWIN__
#include <windows.h>
#include <io.h>
#elif defined( _WIN32)
#include <windows.h>
#include <io.h>
#include <share.h>
#include <direct.h>
#endif

#include <assert.h>
#include <array>
#include <cstdint>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef _WIN32
#include <unistd.h>
#include <sys/time.h>
#endif

#include <algorithm>
#include <vector>

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/sysctl.h>
#elif defined(__SVR4) && defined(__sun)
#include <unistd.h>
#include <sys/loadavg.h>
#elif defined(_AIX) && !defined(__PASE__)
#include <libperfstat.h>
#elif defined(__linux__) || defined(__GLIBC__)
#include <sys/sysinfo.h>
#include <fstream>
#include <map>
#include "string_piece_util.h"
#endif

#if defined(__FreeBSD__)
#include <sys/cpuset.h>
#endif

#include "edit_distance.h"

#include <immintrin.h>
#include <cstring>

#if defined(__linux__)
#define NEEDS_BMI2_INTRINSICS __attribute__((target("bmi2")))
#else
#define NEEDS_BMI2_INTRINSICS
#endif

using namespace std;

namespace {

const std::uint64_t msb{ 0x80'80'80'80'80'80'80'80ull };
const std::uint64_t lsb{ 0x01'01'01'01'01'01'01'01ull };

#ifdef _WIN32
#pragma intrinsic(_BitScanForward64,_BitScanReverse64)
#endif

bool bit_scan_forward64(unsigned long* index, std::uint64_t x) {
#ifdef _WIN32
 return _BitScanForward64(index, x) != 0;
#else
    if (x == 0) return false;
    *index = __builtin_ctzll(x);
    return true;
#endif
}

bool bit_scan_reverse64(unsigned long* index, std::uint64_t x) {
#ifdef _WIN32
 return _BitScanReverse64(index, x) != 0;
#else
  if (x == 0) return false;
  *index = 63 - __builtin_clzll(x);
  return true;
#endif
}

int popcnt64(std::uint64_t x) {
#ifdef _WIN32
  return __popcnt64(x);
#else
  return __builtin_popcountll(x);
#endif
}

std::uint64_t equal(std::uint64_t lhs, std::uint8_t c) {
  const std::uint64_t rhs = 0x0101010101010101ull * c;
  const std::uint64_t zero_if_equal = lhs ^ rhs;
  return ~(zero_if_equal | ((zero_if_equal | msb) - lsb));
}

// Set MSB for each char to 1 if it equals 'c'
std::array<std::uint64_t, 8> msb_equal(const std::uint64_t* lhs,
                                   std::uint8_t c) {
  const std::uint64_t rhs = 0x0101010101010101ull * c;
  const std::uint64_t zero_if_equal[] = {
    lhs[0] ^ rhs, lhs[1] ^ rhs, lhs[2] ^ rhs, lhs[3] ^ rhs,
    lhs[4] ^ rhs, lhs[5] ^ rhs, lhs[6] ^ rhs, lhs[7] ^ rhs,
  };
  const std::array<std::uint64_t, 8> result = {
    // Does this work? see memchr.c
    // https://github.com/lattera/glibc/blob/master/string/memchr.c
  //(~(zero_if_equal[I] | ((zero_if_equal[I] | msb) - lsb)) & msb)
#define EQUAL_TEST(I) \
    (zero_if_equal[I] - lsb) & ~zero_if_equal[I]
    EQUAL_TEST(0), EQUAL_TEST(1), EQUAL_TEST(2), EQUAL_TEST(3),
    EQUAL_TEST(4), EQUAL_TEST(5), EQUAL_TEST(6), EQUAL_TEST(7),
  };
  return result;
}

NEEDS_BMI2_INTRINSICS 
std::uint64_t compress(const std::uint64_t *v) {
#if 1
  const std::uint64_t bits =
  _pext_u64(v[0], 0x80'80'80'80'80'80'80'80ull) |
  _pext_u64(v[1], 0x80'80'80'80'80'80'80'80ull) << 8 |
  _pext_u64(v[2], 0x80'80'80'80'80'80'80'80ull) << 16 |
  _pext_u64(v[3], 0x80'80'80'80'80'80'80'80ull) << 24 |
  _pext_u64(v[4], 0x80'80'80'80'80'80'80'80ull) << 32 |
  _pext_u64(v[5], 0x80'80'80'80'80'80'80'80ull) << 40 |
  _pext_u64(v[6], 0x80'80'80'80'80'80'80'80ull) << 48 |
  _pext_u64(v[7], 0x80'80'80'80'80'80'80'80ull) << 56;
    return bits;
#else
  const std::uint64_t MAGIC = 0x02'04'08'10'20'40'81ull;
  const std::uint64_t bits =
    (((v[0] & msb) * MAGIC) >> 56) |
    (((v[1] & msb) * MAGIC) >> 56) << 8 |
    (((v[2] & msb) * MAGIC) >> 56) << 16 |
    (((v[3] & msb) * MAGIC) >> 56) << 24 |
    (((v[4] & msb) * MAGIC) >> 56) << 32 |
    (((v[5] & msb) * MAGIC) >> 56) << 40 |
    (((v[6] & msb) * MAGIC) >> 56) << 48 |
    (((v[7] & msb) * MAGIC) >> 56) << 56;
    return bits;
#endif
}

void get_slashdot(const std::uint64_t* lhs,
                  std::uint64_t* forwardslashes, std::uint64_t* dots) {
  const std::uint64_t rhs = lsb * '.';
  const std::uint64_t is_lsb_set[] = {
    (lhs[0] & lsb) << 7, (lhs[1] & lsb) << 7, (lhs[2] & lsb) << 7,
    (lhs[3] & lsb) << 7, (lhs[4] & lsb) << 7, (lhs[5] & lsb) << 7,
    (lhs[6] & lsb) << 7, (lhs[7] & lsb) << 7,
  };
  // compare with the lsb set to 0 to test for slashes and dots
  // at the same time
  const std::uint64_t mask = ~lsb;
  const std::uint64_t zero_if_equal[] = {
    (lhs[0] & mask) ^ rhs, (lhs[1] & mask) ^ rhs, (lhs[2] & mask) ^ rhs,
    (lhs[3] & mask) ^ rhs, (lhs[4] & mask) ^ rhs, (lhs[5] & mask) ^ rhs,
    (lhs[6] & mask) ^ rhs, (lhs[7] & mask) ^ rhs,
  };

  const std::uint64_t result[] = {
    (zero_if_equal[0] - lsb) & ~zero_if_equal[0],
    (zero_if_equal[1] - lsb) & ~zero_if_equal[1],
    (zero_if_equal[2] - lsb) & ~zero_if_equal[2],
    (zero_if_equal[3] - lsb) & ~zero_if_equal[3],
    (zero_if_equal[4] - lsb) & ~zero_if_equal[4],
    (zero_if_equal[5] - lsb) & ~zero_if_equal[5],
    (zero_if_equal[6] - lsb) & ~zero_if_equal[6],
    (zero_if_equal[7] - lsb) & ~zero_if_equal[7],
  };

  const std::uint64_t slash_or_dot = compress(result);
  const std::uint64_t low_bit_set = compress(is_lsb_set);
  *forwardslashes = slash_or_dot & low_bit_set;
  *dots = slash_or_dot & ~low_bit_set;
}

void get_slashdot(const std::array<std::uint64_t, 8>& lhs,
                  std::uint64_t* forwardslashes, std::uint64_t* dots) {
  get_slashdot(lhs.data(), forwardslashes, dots);
}

std::uint64_t convert_backslashes_msb(const std::uint64_t text, const std::uint64_t backslashes_msb) {
  return text ^ (backslashes_msb >> 7) * 0x73ull;
}

void convert_backslashes(std::uint64_t* text,
                         const std::uint64_t* backslashes_msb) {
#define EQUAL_TEST(I) text[I] ^= (backslashes_msb[I] >> 7) * 0x73ull;

  EQUAL_TEST(0);
  EQUAL_TEST(1);
  EQUAL_TEST(2);
  EQUAL_TEST(3);
  EQUAL_TEST(4);
  EQUAL_TEST(5);
  EQUAL_TEST(6);
  EQUAL_TEST(7);
}

void disambiguation(char* path, std::size_t* len, std::uint64_t* slash_bits) {
  // Disambiguate between overloads of CanonicalizePath
  CanonicalizePath(path, len, slash_bits);
}

/*
What's the plan?

We need to remove
  1. Empty paths
  2. Current paths
 * (.)
  3. Parent paths (..)

(And keep the initial slashes for absolute
 * paths).

Assume that every path ends with a slash to make things easier

  1.
 * Empty paths

  This can be detected by by doing equality with / and then
 * shifting and bitwise and to detect

  2. Current paths

  This can be
 * detected by doing equality with . and slashes, then shifting and bitwise to
 * detect

  3. Parent paths

  This can be detected the same way, but we need
 * to be able to remove the previous path as well,
  in this case we could just
 * do a non SWAR solution - but that feels inelegant.

  So we can look at the
 * slashes we intend *not* to remove and we need to see if we can "fill in the

 * gaps" between any parent paths and the slashes we intend to keep.


 * "abc/defghi/../zy/"
  "00010000001001001" slashes (A)
  "00000000000001000"
 * parent path (B)
  "00010000000000000" slashes to remove (C)


 * "00000000000001111" B | (B - 1)
  "00011111111111111" C | (C - 1) 


 * "00011111111110000" to remove (WANT) = C & ~B

  */


}

void Fatal(const char* msg, ...) {
  va_list ap;
  fprintf(stderr, "ninja: fatal: ");
  va_start(ap, msg);
  vfprintf(stderr, msg, ap);
  va_end(ap);
  fprintf(stderr, "\n");
#ifdef _WIN32
  // On Windows, some tools may inject extra threads.
  // exit() may block on locks held by those threads, so forcibly exit.
  fflush(stderr);
  fflush(stdout);
  ExitProcess(1);
#else
  exit(1);
#endif
}

void Warning(const char* msg, va_list ap) {
  fprintf(stderr, "ninja: warning: ");
  vfprintf(stderr, msg, ap);
  fprintf(stderr, "\n");
}

void Warning(const char* msg, ...) {
  va_list ap;
  va_start(ap, msg);
  Warning(msg, ap);
  va_end(ap);
}

void Error(const char* msg, va_list ap) {
  fprintf(stderr, "ninja: error: ");
  vfprintf(stderr, msg, ap);
  fprintf(stderr, "\n");
}

void Error(const char* msg, ...) {
  va_list ap;
  va_start(ap, msg);
  Error(msg, ap);
  va_end(ap);
}

void Info(const char* msg, va_list ap) {
  fprintf(stdout, "ninja: ");
  vfprintf(stdout, msg, ap);
  fprintf(stdout, "\n");
}

void Info(const char* msg, ...) {
  va_list ap;
  va_start(ap, msg);
  Info(msg, ap);
  va_end(ap);
}

void CanonicalizePath(string* path, uint64_t* slash_bits) {
  std::size_t len = path->size();
  if (len > 0) {
    char* str = &(*path)[0];
    CanonicalizePath(str, &len, slash_bits);
    path->erase(path->begin() + len, path->end());
  }
}

static bool IsPathSeparator(char c) {
#ifdef _WIN32
  return c == '/' || c == '\\';
#else
  return c == '/';
#endif
}

void CanonicalizePath(char* path, size_t* len, uint64_t* slash_bits) {
  // WARNING: this function is performance-critical; please benchmark
  // any changes you make to it.
  if (*len == 0) {
    return;
  }

  char* start = path;
  char* dst = start;
  char* dst_start = dst;
  const char* src = start;
  const char* end = start + *len;
  const char* src_next;

  // For absolute paths, skip the leading directory separator
  // as this one should never be removed from the result.
  if (IsPathSeparator(*src)) {
#ifdef _WIN32
    // Windows network path starts with //
    if (src + 2 <= end && IsPathSeparator(src[1])) {
      src += 2;
      dst += 2;
    } else {
      ++src;
      ++dst;
    }
#else
    ++src;
    ++dst;
#endif
    dst_start = dst;
  } else {
    // For relative paths, skip any leading ../ as these are quite common
    // to reference source files in build plans, and doing this here makes
    // the loop work below faster in general.
    while (src + 3 <= end && src[0] == '.' && src[1] == '.' &&
           IsPathSeparator(src[2])) {
      src += 3;
      dst += 3;
    }
  }

  // Loop over all components of the paths _except_ the last one, in
  // order to simplify the loop's code and make it faster.
  int component_count = 0;
  char* dst0 = dst;
  for (; src < end; src = src_next) {
#ifndef _WIN32
    // Use memchr() for faster lookups thanks to optimized C library
    // implementation. `hyperfine canon_perftest` shows a significant
    // difference (e,g, 484ms vs 437ms).
    const char* next_sep =
        static_cast<const char*>(::memchr(src, '/', end - src));
    if (!next_sep) {
      // This is the last component, will be handled out of the loop.
      break;
    }
#else
    // Need to check for both '/' and '\\' so do not use memchr().
    // Cannot use strpbrk() because end[0] can be \0 or something else!
    const char* next_sep = src;
    while (next_sep != end && !IsPathSeparator(*next_sep))
      ++next_sep;
    if (next_sep == end) {
      // This is the last component, will be handled out of the loop.
      break;
    }
#endif
    // Position for next loop iteration.
    src_next = next_sep + 1;
    // Length of the component, excluding trailing directory.
    size_t component_len = next_sep - src;

    if (component_len <= 2) {
      if (component_len == 0) {
        continue;  // Ignore empty component, e.g. 'foo//bar' -> 'foo/bar'.
      }
      if (src[0] == '.') {
        if (component_len == 1) {
          continue;  // Ignore '.' component, e.g. './foo' -> 'foo'.
        } else if (src[1] == '.') {
          // Process the '..' component if found. Back up if possible.
          if (component_count > 0) {
            // Move back to start of previous component.
            --component_count;
            while (--dst > dst0 && !IsPathSeparator(dst[-1])) {
              // nothing to do here, decrement happens before condition check.
            }
          } else {
            dst[0] = '.';
            dst[1] = '.';
            dst[2] = src[2];
            dst += 3;
          }
          continue;
        }
      }
    }
    ++component_count;

    // Copy or skip component, including trailing directory separator.
    if (dst != src) {
      ::memmove(dst, src, src_next - src);
    }
    dst += src_next - src;
  }

  // Handling the last component that does not have a trailing separator.
  // The logic here is _slightly_ different since there is no trailing
  // directory separator.
  size_t component_len = end - src;
  do {
    if (component_len == 0)
      break;  // Ignore empty component (e.g. 'foo//' -> 'foo/')
    if (src[0] == '.') {
      if (component_len == 1)
        break;  // Ignore trailing '.' (e.g. 'foo/.' -> 'foo/')
      if (component_len == 2 && src[1] == '.') {
        // Handle '..'. Back up if possible.
        if (component_count > 0) {
          while (--dst > dst0 && !IsPathSeparator(dst[-1])) {
            // nothing to do here, decrement happens before condition check.
          }
        } else {
          dst[0] = '.';
          dst[1] = '.';
          dst += 2;
          // No separator to add here.
        }
        break;
      }
    }
    // Skip or copy last component, no trailing separator.
    if (dst != src) {
      ::memmove(dst, src, component_len);
    }
    dst += component_len;
  } while (0);

  // Remove trailing path separator if any, but keep the initial
  // path separator(s) if there was one (or two on Windows).
  if (dst > dst_start && IsPathSeparator(dst[-1]))
    dst--;

  if (dst == start) {
    // Handle special cases like "aa/.." -> "."
    *dst++ = '.';
  }

  *len = dst - start;  // dst points after the trailing char here.
#ifdef _WIN32
  uint64_t bits = 0;
  uint64_t bits_mask = 1;

  for (char* c = start; c < start + *len; ++c) {
    switch (*c) {
      case '\\':
        bits |= bits_mask;
        *c = '/';
        NINJA_FALLTHROUGH;
      case '/':
        bits_mask <<= 1;
    }
  }

  *slash_bits = bits;
#else
  *slash_bits = 0;
#endif
}

void CanonicalizePathTwiceMemChr(char* path, size_t* len, uint64_t* slash_bits) {
  // WARNING: this function is performance-critical; please benchmark
  // any changes you make to it.
  if (*len == 0) {
    return;
  }

  char* start = path;
  char* dst = start;
  char* dst_start = dst;
  const char* end = start + *len;

  // For absolute paths, skip the leading directory separator
  // as this one should never be removed from the result.
  if (IsPathSeparator(*dst)) {
#ifdef _WIN32
    // Windows network path starts with //
    if (end - dst >= 2 && IsPathSeparator(dst[1])) {
      dst += 2;
    } else {
      ++dst;
    }
#else
    ++dst;
#endif
    dst_start = dst;
  } else {
    // For relative paths, skip any leading ../ as these are quite common
    // to reference source files in build plans, and doing this here makes
    // the loop work below faster in general.
    while (end - dst >= 3 && dst[0] == '.' && dst[1] == '.' &&
           IsPathSeparator(dst[2])) {
      dst += 3;
    }
  }

  const char* src = dst;
  const char* src_next;
#ifdef _WIN32
  // Keep track of all slashes we may have seen but not tracked whether
  // it was backslashes or forwardslashes for calculating `slash_bits`.
  const char* unknown_slashes_end = dst;

  // Track next forwardslashes and backslashes on Windows. Do an initial
  // lookup for the first backslash as we can use this to potentiallyP
  // skip work setting `slash_bits`.
  const char* first_fs =
      static_cast<const char*>(::memchr(src, '/', end - src));
  const char* next_fs = first_fs ? first_fs : end;
  const char* first_bs =
      static_cast<const char*>(::memchr(src, '\\', end - src));
  const char* next_bs = first_bs ? first_bs : end;
#endif

  // Keep track of characters that can't be removed by ".." components
  char* dst0 = dst;

  // Batch up copies of contiguous data [to_copy_from, src) to reduce the
  // number of calls to memmove.
  const char* to_copy_from = src;
  for (; src != end; src = src_next) {
#ifndef _WIN32
    // Use memchr() for faster lookups thanks to optimized C library
    // implementation. `hyperfine canon_perftest` shows a significant
    // difference (e,g, 484ms vs 437ms).
    const char* next_sep =
        static_cast<const char*>(::memchr(src, '/', end - src));

    // Position for next loop iteration.
    const std::size_t component_len = next_sep ? next_sep - src : end - src;
    // Length of the component, excluding trailing directory.
    src_next = next_sep ? next_sep + 1 : end;
#else
    // Multiple calls to `memchr` are much more efficient than manually
    // looping and checking for both '/' and '\\'.
    if (!next_bs) {
      next_bs = static_cast<const char*>(::memchr(src, '\\', end - src));
      next_bs = next_bs ? next_bs : end;
    }
    if (!next_fs) {
      next_fs = static_cast<const char*>(::memchr(src, '/', end - src));
      next_fs = next_fs ? next_fs : end;
    }
    const char*& next_sep = next_fs < next_bs ? next_fs : next_bs;

    // Position for next loop iteration.
    src_next = next_sep != end ? next_sep + 1 : end;
    // Length of the component, excluding trailing directory.
    const std::size_t component_len = next_sep - src;

    // Reset either `next_fs` or `next_bs` to search in the next iteration.
    next_sep = nullptr;
#endif

    // Handle the common case first.
    if (component_len > 2) {
      continue;
    }

    // Ignore empty component, e.g. "foo//bar" -> "foo/bar".
    if (component_len > 0) {
      if (src[0] != '.') {
        // A non-special path component of length 1 or 2, e.g. "a/" or "ab/".
        continue;
      }

      // Ignore '.' component, e.g. "./foo" -> "foo".
      if (component_len == 2) {
        if (src[1] != '.') {
          // A non-special, hidden path component of length 2, e.g. ".a/".
          continue;
        }
        
        // See if we have a component to backup
        if (dst + (src - to_copy_from) > dst0) {
          if (src != to_copy_from) {
            // We have something in the copy buffer and can back this up, then
            // copy that to dst.
            while (--src > dst0 && !IsPathSeparator(src[-1]));
          } else {
            // We have nothing in the pending copy buffer and have to back
            // up dst.
            while (--dst > dst0 && !IsPathSeparator(dst[-1]));
            to_copy_from = src_next;
            continue;
          }
        } else {
          // If we have a ".." component that we can't back up then
          // add it to the characters to copy and adjust `dst0` since
          // we can't remove it later.
          dst0 += src_next - src;
          src = src_next;
        }
      }
    }

    // If we get here then we have a component that we don't want to
    // copy (e.g. "./") so we copy the accumulated data up to this point
    // and start the next batch after this component.
    ::memmove(dst, to_copy_from, src - to_copy_from);
    dst += src - to_copy_from;
    to_copy_from = src_next;
  }

  // Copy any remaining data.
  if (dst != to_copy_from) {
    ::memmove(dst, to_copy_from, src - to_copy_from);
  }
  dst += src - to_copy_from;

  // Remove trailing path separator if any, but keep the initial
  // path separator(s) if there was one (or two on Windows).
  if (dst > dst_start && IsPathSeparator(dst[-1]))
    dst--;

  if (dst == start) {
    // Handle special cases like "aa/.." -> "."
    *dst++ = '.';
  }

  *len = dst - start;  // dst points after the trailing char here.
#ifdef _WIN32
  // Fast path for all forwardslashes
  if (!first_bs && !::memchr(start, '\\', unknown_slashes_end - start)) {
    *slash_bits = 0;
    return;
  }

  // Medium-slow path for all backslashes
  if (!first_fs && !::memchr(start, '/', unknown_slashes_end - start)) {
    char* bs = static_cast<char*>(::memchr(start, '\\', dst - start));
    std::size_t bs_count = 0;
    while (bs) {
      ++bs_count;
      *bs++ = '/';
      bs = static_cast<char*>(::memchr(bs, '\\', dst - bs));
    }
    *slash_bits = bs_count >= 64
                      ? ~std::uint64_t(0)
                      : (static_cast<std::uint64_t>(1) << (bs_count)) - 1;
    return;
  }

  // Slow path for a mixture of slashes
  std::uint64_t bits = 0;
  std::uint64_t bits_mask = 1;
  for (char* c = start; c != dst; ++c) {
    switch (*c) {
      case '\\':
        bits |= bits_mask;
        *c = '/';
        NINJA_FALLTHROUGH;
      case '/':
        bits_mask <<= 1;
    }
  }

  *slash_bits = bits;
#else
  *slash_bits = 0;
#endif
}

const std::uint64_t all_dots = lsb * '.';
const std::uint64_t mask = ~lsb;

NEEDS_BMI2_INTRINSICS 
std::uint64_t get_slashdot_indicator(const std::uint64_t word) {
  const std::uint64_t zero_if_equal = (word & mask) ^ all_dots;
  const std::uint64_t corrected = (zero_if_equal - lsb) & ~zero_if_equal;
  const std::uint64_t bits =
      _pext_u64(corrected, 0x80'80'80'80'80'80'80'80ull);
  return bits;
}

struct CountAndValue {
  int count = 0;
  bool value = false;
};

struct Biterator {
  std::uint64_t bits_;
  unsigned total_ = 0;
  CountAndValue value_{};

  explicit Biterator(std::uint64_t bits)
      : bits_(bits) {
    // Inspect the first bit once.
    value_.value = (bits_ & 1ull) != 0;
    read_next();
  }

  void read_next() {
    assert(total_ != 64);
    /*
    if (total_ == 64) {
      value_ = {};
      return;
    }
    */

    const std::uint64_t remaining_bits = bits_ >> total_;
    const unsigned bits_left = 64 - total_;

    const std::uint64_t value_mask =
        0ull - static_cast<std::uint64_t>(value_.value);
    const std::uint64_t changed = remaining_bits ^ value_mask;

    unsigned long first_changed_bit;
    const bool found_changed_bit =
        bit_scan_forward64(&first_changed_bit, changed);

    const unsigned count =
        found_changed_bit
            ? static_cast<unsigned>(first_changed_bit)
            : bits_left;

    assert(count > 0);
    assert(count <= bits_left);

    value_.count = count;
    total_ += count;
    value_.value = !value_.value;
  }

  Biterator& operator++() {
    read_next();
    return *this;
  }

  CountAndValue operator*() const {
    CountAndValue result = value_;
    result.value = !result.value; // because read_next pre-flipped for next time
    return result;
  }
};

struct EndSentinel {};

bool operator==(const Biterator& lhs, EndSentinel) {
  return lhs.total_ == 64;
}

bool operator!=(const Biterator& lhs, EndSentinel rhs) {
  return !(lhs == rhs);
}

struct BitRange {
  std::uint64_t bits_;
  unsigned long skip_start_;
  BitRange(std::uint64_t bits) : bits_(bits), skip_start_(0) {}

  Biterator begin() { return Biterator(bits_); }

  EndSentinel end() { return EndSentinel(); }
};

struct ChunkedReader {
  char* src_;
  std::size_t remaining_;
  bool keep_going_;

  ChunkedReader(char* string, std::size_t len)
      : src_(string), remaining_(len), keep_going_(len > 0) {}

  /// Read at most \a buffer_size bytes into \a buffer and increment
  /// the read pointer by the number of bytes read.  Return a pointer
  /// to the next bytes to read, or \c nullptr if there are no more bytes to
  /// read.
  char *read(char *buffer, std::size_t* buffer_size) {
    assert(*buffer_size <= 64);
    if (!keep_going_) {
      return nullptr;
    }

    const std::size_t buffer_capacity = *buffer_size;
    if (remaining_ >= buffer_capacity) {
      std::memcpy(buffer, src_, buffer_capacity);
    } else {
      *buffer_size = remaining_;
      std::memcpy(buffer, src_, remaining_);
      const char padding[64] =
          "/"
          "\0\0\0\0\0\0\0\0"
          "\0\0\0\0\0\0\0\0"
          "\0\0\0\0\0\0\0\0"
          "\0\0\0\0\0\0\0\0"
          "\0\0\0\0\0\0\0\0"
          "\0\0\0\0\0\0\0\0"
          "\0\0\0\0\0\0\0\0"
          "\0\0\0\0\0\0";
      std::memcpy(buffer + remaining_, padding,
                  buffer_capacity - remaining_ - 1);
      keep_going_ = false;
    }
    src_ += *buffer_size;
    return src_;
  }

};

struct InPlaceStringModifier {
  // "abc"
  // all start at a
  // if we keep (1), then move src forward 1 and dst forward 1
  // if we skip (1), then move src forward 1 and dst stays
  // if we then keep(1), we move src forward 1

  // "abcdjsk;fdiofsaifs"
  //      ^ dst
  //          ^ src
  //        ^ pending_copy_from
  // Output is
  // [dst, pending_copy_from)
          
  char* out_; /// Where we are writing to.
  const char* pending_copy_from_;
  char* in_; /// Where we are reading from.
  char* end_;

  /// Read from and write to the \a string of length \a len.
  explicit InPlaceStringModifier(char* string, std::size_t len)
      : out_(string), in_(string), pending_copy_from_(string), end_(string + len) {}

  /// Write \a count bytes from the current read position to the current write
  /// position.  Increment both the read and write positions by \a count bytes.
  void keep(std::size_t count) {
    if (count && (out_ != in_)) {
      ::memmove(out_, in_, count);
    }
    out_ += count;
    in_ += count;
    // We don't need to do anything
    //if (pending_copy_from_ != dst_) {
      //::memmove(dst_, pending_copy_from_, src_ - pending_copy_from_);
    //}
    //src_ += count;
    //dst_ += count;
    //pending_copy_from_ = src_;
  }

  /// Increment the read position by \a length bytes, effectively skipping
  /// that many bytes without copying them to the out position.
  void remove(std::size_t count) {
    in_ += count;
  }

  /// Undo writing the last \a count bytes, rewinding the write
  /// position.
  void undo(std::size_t count) {
    out_ -= count;
  }

  /// Return the current write position.
  char* write_pos() { return out_; }

  /// TODO
  char *flush() {
    //::memmove(dst_, pending_copy_from_, pending_count_);
    keep(end_ - in_);
    return out_;
  }
};

void CanonicalizePath2(string* path, uint64_t* slash_bits) {
  std::size_t len = path->size();
  if (len > 0) {
    char* str = &(*path)[0];
    CanonicalizePath2(str, &len, slash_bits);
    path->erase(path->begin() + len, path->end());
  }
}

NEEDS_BMI2_INTRINSICS 
std::uint64_t get_slashdot_indicator(const std::uint64_t* buffer, std::size_t count) {
  const std::uint64_t all_dots = lsb * '.';
  const std::uint64_t mask = ~lsb;
  std::uint64_t slashdot_indicator = 0;
  for (int i = 0; i < count; ++i) {
    const std::uint64_t zero_if_equal =
        (buffer[i] & mask) ^ all_dots;
    const std::uint64_t corrected = (zero_if_equal - lsb) & ~zero_if_equal;
    const std::uint64_t bits = _pext_u64(corrected, msb);
    slashdot_indicator |= bits << (i * 8);
  }
  return slashdot_indicator;
}

std::uint64_t convert_backslashes(std::uint64_t* buffer, std::size_t count) {
  const std::uint64_t backslashes = lsb * '\\';
  std::uint64_t any_equal = 0;
  std::uint64_t backslashes_msb[8];
  for (int i = 0; i < count; ++i) {
    const std::uint64_t zero_if_equal = buffer[i] ^ backslashes;
    const std::uint64_t result = (zero_if_equal - lsb) & ~zero_if_equal;
    backslashes_msb[i] = result;
    any_equal |= result;
  }

  // Convert backslashes if we have any
  std::uint64_t backslash_bits = 0;
  if (any_equal & msb) {
    for (int i = 0; i < count; ++i) {
      const std::uint64_t tmp = backslashes_msb[i] & msb;
      buffer[i] ^= (tmp >> 7) * 0x73ull;
      const std::uint64_t bits =
          _pext_u64(tmp, 0x80'80'80'80'80'80'80'80ull);
      backslash_bits |= bits << (i * 8);
    }
  }
  return backslash_bits;
}

NEEDS_BMI2_INTRINSICS 
std::uint64_t get_lsb_indicator(const std::uint64_t* buffer,
                                std::size_t count) {
  std::uint64_t lsb_indicator = 0;
  for (int i = 0; i < count; ++i) {
    const std::uint64_t bits = _pext_u64((buffer[i] & lsb) << 7, msb);
    lsb_indicator |= bits << (i * 8);
  }
  return lsb_indicator;
}

NEEDS_BMI2_INTRINSICS 
std::uint64_t get_backslash_indicator(std::uint64_t* buffer,
                                      std::size_t count,
                                      std::uint64_t* backslashes_msb) {
  std::uint64_t backslash_indicator = 0;
  for (int i = 0; i < count; ++i) {
    backslashes_msb[i] &= msb;
    buffer[i] ^= (backslashes_msb[i] >> 7) * 0x73ull;
    const std::uint64_t bits =
        _pext_u64(backslashes_msb[i], 0x80'80'80'80'80'80'80'80ull);
    backslash_indicator |= bits << (i * 8);
  }
}

NEEDS_BMI2_INTRINSICS 
void CanonicalizePath2(char* path, std::size_t* len, std::uint64_t* slash_bit) {
#define NEED_BACKSLASH 1
  const char* dst_start = path;

  std::uint64_t previous_slashes = static_cast<std::uint64_t>(1) << 63;
  std::uint64_t previous_dots = 0;

  std::uint64_t output_slashes = 0;
  std::uint64_t slash_count = 0;

  // Keep a bitmask for characters that are mutable (removable by "..")
  std::uint64_t mutable_chars = ~static_cast<std::uint64_t>(0);

  // Preserve the initial slash (or double slash on windows)
  if (*len >= 1 && IsPathSeparator(path[0])) {
    if (*len >= 2 && IsPathSeparator(path[1])) {
      mutable_chars = ~static_cast<std::uint64_t>(0b11);
      dst_start += 2;
    }
    else {
      mutable_chars = ~static_cast<std::uint64_t>(0b1);
      dst_start += 1;
    }
  }

  // Track the start of a contiguous region we haven't copied yet.
  // This lets us batch fast-path chunks and only call memmove when
  // we actually encounter characters to remove.
  InPlaceStringModifier writer(path, *len);
  ChunkedReader reader(path, *len);

  std::uint64_t buffer[8];
  std::size_t chunk_size = sizeof(buffer);
  while (char *next = reader.read(reinterpret_cast<char *>(&buffer), &chunk_size)) {
    const std::uint64_t padding_to_remove =
        chunk_size == 64
            ? 0
            : ~((static_cast<std::uint64_t>(1) << (chunk_size)) - 1);
    const std::size_t words_used = (chunk_size / 8) + 1;

    std::uint64_t slashdot_indicator =
        get_slashdot_indicator(buffer, words_used);

#if NEED_BACKSLASH
    // TODO: Can we do this first, then not generate the indicator, then just
    // generate it from moving get_slashdot_indicator afterwards?
    const std::uint64_t backslash_indicator =
        convert_backslashes(buffer, words_used);
    slashdot_indicator |= backslash_indicator;
    if (backslash_indicator) {
      std::memcpy(next - chunk_size, buffer, chunk_size);
    }
#endif

    // Quick exit if we don't need to update slash_bits
#if !NEED_BACKSLASH
    if ((slashdot_indicator & (slashdot_indicator << 1)) == 0) {
      src += chunk_size;
      mutable_chars = ~static_cast<std::uint64_t>(0);
      TODO: previous_chars
      continue;
    }
#endif

    const std::uint64_t lsb_indicator = get_lsb_indicator(buffer, words_used);
    const std::uint64_t forwardslash_bits = slashdot_indicator & lsb_indicator;
    const std::uint64_t dot_bits = slashdot_indicator & ~lsb_indicator;

    const std::uint64_t slash_bits =
#if NEED_BACKSLASH
        backslash_indicator |
#endif
        forwardslash_bits;

    // Keep track of characters to remove
    std::uint64_t to_remove = padding_to_remove;

#if NEED_BACKSLASH
    // Quick exit if we do need to update slash_bits
    const std::uint64_t previous_slashdot = previous_slashes | previous_dots;
    if ((slashdot_indicator & ((slashdot_indicator << 1) | (previous_slashdot >> 63))) == 0) {
      mutable_chars = ~static_cast<std::uint64_t>(0);
      const std::uint64_t to_keep = ~to_remove;
      if (slash_count < 64) {
        output_slashes |=
            _pext_u64(to_keep & backslash_indicator, to_keep & slash_bits)
            << slash_count;
      }
      slash_count += popcnt64(to_keep & slash_bits);
      previous_slashes = slash_bits;
      previous_dots = dot_bits;
      continue;
    }
#endif

    // Look at empty paths (bit set for each slash with a preceeding slash)
    const std::uint64_t empty_paths_to_remove =
        slash_bits & ((slash_bits << 1u) | (previous_slashes >> 63));
    to_remove |= empty_paths_to_remove;

    // Look at current path /./ (bit set on the last slash)
    const std::uint64_t current_path_indicator =
        ((slash_bits << 2u) | (previous_slashes >> 62)) &
        ((dot_bits << 1u) | (previous_dots >> 63)) & slash_bits;
    const std::uint64_t current_path_to_remove =
        current_path_indicator | (current_path_indicator >> 1u);
    if (current_path_indicator == 1) {
      // In this case we have a "/./" that spans across 2 blocks and we need to
      // remove the "." that is at the end of the last block
      writer.undo(1);
    }
    to_remove |= current_path_to_remove;

    // Look at parent path /../ (bit set on the last slash)
    const std::uint64_t parent_path_indicator =
        ((slash_bits << 3u) | (previous_slashes >> 61)) &
        ((dot_bits << 2u) | (previous_dots >> 62)) &
        ((dot_bits << 1u) | (previous_dots >> 63)) &
      slash_bits;

    // For each parent path, find and mark the previous directory for removal
    std::uint64_t remaining_parent = parent_path_indicator;
    while (remaining_parent) {
      const std::int8_t first_parent_path_indicator = [&] {
        unsigned long bit_pos;
        [[maybe_unused]] const bool okay =
            bit_scan_forward64(&bit_pos, remaining_parent);
        assert(okay);
        return bit_pos;
      }();

      // Trying to find the previous slash to the "../" parent
      // path, ignoring slashes that have already been removed
      const std::uint64_t before_mask =
          (static_cast<std::uint64_t>(1) << first_parent_path_indicator) - 1;
      const std::uint64_t to_consider =
          before_mask & slash_bits & ~to_remove & mutable_chars;
      // Only if there are slashes to consider can we attempt to remove
      // the previous directory.  Otherwise we keep the "../"
      if (!to_consider) {
        const std::uint64_t immutable = (static_cast<std::uint64_t>(0b111)
                                         << (first_parent_path_indicator - 2));
        const std::uint64_t mask = immutable & ~to_remove;
        mutable_chars &= ~mask;
      } else {
        const std::int8_t prev_slash1 = [&] {
          unsigned long bit_pos;
          [[maybe_unused]] const bool okay = bit_scan_reverse64(&bit_pos, to_consider);
          assert(okay);
          return bit_pos;
        }();

        const std::uint64_t before_mask2 =
            (static_cast<std::uint64_t>(1) << prev_slash1) - 1;

        const auto bits_between = [](std::int8_t start, std::int8_t end) {
          // end is one past the end
          assert(start <= end);
          assert(end <= 64);
          const std::uint64_t ones = ~static_cast<std::uint64_t>(0);
          return (ones << start) & (ones >> (64 - end));
        };

        bool found;
        const std::int8_t prev_slash2 = [&] {
          unsigned long bit_pos;
          // Here we may not find a slash if it's the start of the path
          found = bit_scan_reverse64(&bit_pos, to_consider & before_mask2);
          return found ? bit_pos : 0;
        }();

        const std::uint64_t dots_and_prev_directory_to_remove =
            bits_between(prev_slash2, first_parent_path_indicator + !found);
        to_remove |= dots_and_prev_directory_to_remove;
      }
      remaining_parent &=
          ~(static_cast<std::uint64_t>(1) << first_parent_path_indicator);
    }

    const std::uint64_t to_keep = ~to_remove;

    // Calculate slash_bits
#if NEED_BACKSLASH
    if (slash_count < 64) {
      output_slashes |=
          _pext_u64(to_keep & backslash_bits, to_keep & slash_bits)
          << slash_count;
    }
    slash_count += popcnt64(to_keep & slash_bits);
#endif

    // Finally, copy or skip all the values we need to in the buffer.
    for (const CountAndValue& v : BitRange(mutable_chars & to_remove)) {
      if (v.value)
        writer.remove(v.count);
      else
        writer.keep(v.count);
    }

    previous_slashes = slash_bits;
    previous_dots = dot_bits;
    mutable_chars = ~static_cast<std::uint64_t>(0);
  }

  // Flush any remaining pending copy region.
  char *dst = writer.flush();
  
  // Remove trailing path separator if any, but keep the initial
  // path separator(s) if there was one (or two on Windows).
  if (dst > dst_start && IsPathSeparator(dst[-1]))
    dst--;

  if (dst == path) {
    // Handle special cases like "aa/.." -> "."
    *dst++ = '.';
  }
  *len = dst - path;
  *slash_bit = output_slashes;
}

void CanonicalizePath4(string* path, uint64_t* slash_bits) {
  std::size_t len = path->size();
  if (len > 0) {
    char* str = &(*path)[0];
    CanonicalizePath4(str, &len, slash_bits);
    path->erase(path->begin() + len, path->end());
  }
}

NEEDS_BMI2_INTRINSICS 
void CanonicalizePath4(char* path, std::size_t* len, std::uint64_t* slash_bit) {
  const char* src = path;
  char* dst = path;
  const char* dst_start = dst;
  std::size_t remaining = *len;
  std::uint64_t previous_slashes = 1;
  std::uint64_t previous_dots = 0;

  std::uint64_t output_slashes = 0;
  std::uint64_t slash_count = 0;

  // Keep a bitmask for characters that are mutable (not removable by "..")
  std::uint64_t mutable_chars = ~static_cast<std::uint64_t>(0);

  // Preserve the initial slash (or double slash on windows)
  if (remaining >= 1 && IsPathSeparator(src[0])) {
    if (remaining >= 2 && IsPathSeparator(src[1])) {
      mutable_chars = ~static_cast<std::uint64_t>(0b11);
      dst_start += 2;
    } else {
      mutable_chars = ~static_cast<std::uint64_t>(0b1);
      dst_start += 1;
    }
  }

  // Track the start of a contiguous region we haven't copied yet.
  // This lets us batch fast-path chunks and only call memmove when
  // we actually encounter characters to remove.
  const char* pending_copy_from = src;

  const std::size_t byte_overflow = 0;

  const char* end = src + *len;
  std::uint64_t buffer[8];
  bool is_zero = false;
  while (remaining >= 64) {
    std::memcpy(&buffer, src, sizeof(buffer));

    const std::uint64_t slashdot_indicator =
        get_slashdot_indicator(buffer[0]) |
        (get_slashdot_indicator(buffer[1]) << 8) |
        (get_slashdot_indicator(buffer[2]) << 16) |
        (get_slashdot_indicator(buffer[3]) << 24) |
        (get_slashdot_indicator(buffer[4]) << 32) |
        (get_slashdot_indicator(buffer[5]) << 40) |
        (get_slashdot_indicator(buffer[6]) << 48) |
        (get_slashdot_indicator(buffer[7]) << 56);

    remaining -= 64;
    if (slashdot_indicator & (slashdot_indicator << 1)) {
      is_zero = true;
      continue;
    }
  }

  std::uint64_t slashdot_indicator = 0;
  switch (remaining + 7 / 8) {
  case 7:
    std::memcpy(&buffer[7], src + 56, sizeof(std::uint64_t));
    slashdot_indicator |= get_slashdot_indicator(buffer[7]) << 56;
  case 6:
    std::memcpy(&buffer[6], src + 48, sizeof(std::uint64_t));
    slashdot_indicator |= get_slashdot_indicator(buffer[6]) << 48;
  case 5:
    std::memcpy(&buffer[5], src + 40, sizeof(std::uint64_t));
    slashdot_indicator |= get_slashdot_indicator(buffer[5]) << 40;
  case 4:
    std::memcpy(&buffer[4], src + 32, sizeof(std::uint64_t));
    slashdot_indicator |= get_slashdot_indicator(buffer[4]) << 32;
  case 3:
    std::memcpy(&buffer[3], src + 24, sizeof(std::uint64_t));
    slashdot_indicator |= get_slashdot_indicator(buffer[3]) << 24;
  case 2:
    std::memcpy(&buffer[2], src + 16, sizeof(std::uint64_t));
    slashdot_indicator |= get_slashdot_indicator(buffer[2]) << 16;
  case 1:
    std::memcpy(&buffer[1], src + 8, sizeof(std::uint64_t));
    slashdot_indicator |= get_slashdot_indicator(buffer[1]) << 8;
  case 0:
    std::memcpy(&buffer[0], src, sizeof(std::uint64_t));
    slashdot_indicator |= get_slashdot_indicator(buffer[0]);
  }

  if (slashdot_indicator & (slashdot_indicator << 1)) {
    is_zero = true;
  }

  *len = is_zero ? 0 : *len;
  *slash_bit = 0;
}

void CanonicalizePath3(string* path, uint64_t* slash_bits) {
  std::size_t len = path->size();
  if (len > 0) {
    char* str = &(*path)[0];
    CanonicalizePath3(str, &len, slash_bits);
    path->erase(path->begin() + len, path->end());
  }
}

NEEDS_BMI2_INTRINSICS 
void CanonicalizePath3(char* path, std::size_t* len, std::uint64_t* slash_bit) {
  const char* src = path;
  char* dst = path;
  const char* dst_start = dst;
  std::size_t remaining = *len;
  std::uint64_t previous_slashes = 1;
  std::uint64_t previous_dots = 0;

  std::uint64_t output_slashes = 0;
  std::uint64_t slash_count = 0;

  // Keep a bitmask for characters that are mutable (not removable by "..")
  std::uint64_t mutable_chars = ~static_cast<std::uint64_t>(0);

  // Preserve the initial slash (or double slash on windows)
  if (remaining >= 1 && IsPathSeparator(src[0])) {
    if (remaining >= 2 && IsPathSeparator(src[1])) {
      mutable_chars = ~static_cast<std::uint64_t>(0b11);
      dst_start += 2;
    }
    else {
      mutable_chars = ~static_cast<std::uint64_t>(0b1);
      dst_start += 1;
    }
  }

  // Track the start of a contiguous region we haven't copied yet.
  // This lets us batch fast-path chunks and only call memmove when
  // we actually encounter characters to remove.
  const char* pending_copy_from = src;

  std::size_t words_needed = (*len % 8);
  const std::size_t byte_overflow = 0;

  const char* end = src + *len;
  std::uint64_t buffer[8];
  std::uint64_t slashdot_indicator = 0;
  bool is_zero = false;
  switch (words_needed) {
    while (src < end) {
      slashdot_indicator = 0;
    case 7:
      slashdot_indicator |= get_slashdot_indicator(buffer[0]);
      src += 8;
      std::memcpy(&buffer[1], src, sizeof(std::uint64_t));
    case 6:
      slashdot_indicator |= get_slashdot_indicator(buffer[1]) << 8;
      src += 8;
      std::memcpy(&buffer[2], src, sizeof(std::uint64_t));
    case 5:
      slashdot_indicator |= get_slashdot_indicator(buffer[2]) << 16;
      src += 8;
      std::memcpy(&buffer[3], src, sizeof(std::uint64_t));
    case 4:
      slashdot_indicator |= get_slashdot_indicator(buffer[3]) << 24;
      src += 8;
      std::memcpy(&buffer[4], src, sizeof(std::uint64_t));
    case 3:
      slashdot_indicator |= get_slashdot_indicator(buffer[4]) << 32;
      src += 8;
      std::memcpy(&buffer[5], src, sizeof(std::uint64_t));
    case 2:
      slashdot_indicator |= get_slashdot_indicator(buffer[5]) << 40;
      src += 8;
      std::memcpy(&buffer[6], src, sizeof(std::uint64_t));
    case 1:
      slashdot_indicator |= get_slashdot_indicator(buffer[6]) << 48;
      src += 8;
      std::memcpy(&buffer[7], src, sizeof(std::uint64_t));
    case 0:
      slashdot_indicator |= get_slashdot_indicator(buffer[7]) << 56;
      src += 8;

      if (slashdot_indicator & (slashdot_indicator << 1)) {
        is_zero = true;
        continue;
      } else {
        // Build 
        const std::uint64_t slash_indicator = 0;
        const std::uint64_t dot_indicator = 0;
      }

      words_needed = 7;
    }
  }

  *len = is_zero ? 0 : *len;
  *slash_bit = 0;
}

static inline bool IsKnownShellSafeCharacter(char ch) {
  if ('A' <= ch && ch <= 'Z') return true;
  if ('a' <= ch && ch <= 'z') return true;
  if ('0' <= ch && ch <= '9') return true;

  switch (ch) {
    case '_':
    case '+':
    case '-':
    case '.':
    case '/':
      return true;
    default:
      return false;
  }
}

static inline bool IsKnownWin32SafeCharacter(char ch) {
  switch (ch) {
    case ' ':
    case '"':
      return false;
    default:
      return true;
  }
}

static inline bool StringNeedsShellEscaping(const string& input) {
  for (size_t i = 0; i < input.size(); ++i) {
    if (!IsKnownShellSafeCharacter(input[i])) return true;
  }
  return false;
}

static inline bool StringNeedsWin32Escaping(const string& input) {
  for (size_t i = 0; i < input.size(); ++i) {
    if (!IsKnownWin32SafeCharacter(input[i])) return true;
  }
  return false;
}

void GetShellEscapedString(const string& input, string* result) {
  assert(result);

  if (!StringNeedsShellEscaping(input)) {
    result->append(input);
    return;
  }

  const char kQuote = '\'';
  const char kEscapeSequence[] = "'\\'";

  result->push_back(kQuote);

  string::const_iterator span_begin = input.begin();
  for (string::const_iterator it = input.begin(), end = input.end(); it != end;
       ++it) {
    if (*it == kQuote) {
      result->append(span_begin, it);
      result->append(kEscapeSequence);
      span_begin = it;
    }
  }
  result->append(span_begin, input.end());
  result->push_back(kQuote);
}


void GetWin32EscapedString(const string& input, string* result) {
  assert(result);
  if (!StringNeedsWin32Escaping(input)) {
    result->append(input);
    return;
  }

  const char kQuote = '"';
  const char kBackslash = '\\';

  result->push_back(kQuote);
  size_t consecutive_backslash_count = 0;
  string::const_iterator span_begin = input.begin();
  for (string::const_iterator it = input.begin(), end = input.end(); it != end;
       ++it) {
    switch (*it) {
      case kBackslash:
        ++consecutive_backslash_count;
        break;
      case kQuote:
        result->append(span_begin, it);
        result->append(consecutive_backslash_count + 1, kBackslash);
        span_begin = it;
        consecutive_backslash_count = 0;
        break;
      default:
        consecutive_backslash_count = 0;
        break;
    }
  }
  result->append(span_begin, input.end());
  result->append(consecutive_backslash_count, kBackslash);
  result->push_back(kQuote);
}

int ReadFile(const string& path, string* contents, string* err) {
#ifdef _WIN32
  // This makes a ninja run on a set of 1500 manifest files about 4% faster
  // than using the generic fopen code below.
  err->clear();
  HANDLE f = ::CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
  if (f == INVALID_HANDLE_VALUE) {
    err->assign(GetLastErrorString());
    return -ENOENT;
  }

  for (;;) {
    DWORD len;
    char buf[64 << 10];
    if (!::ReadFile(f, buf, sizeof(buf), &len, NULL)) {
      err->assign(GetLastErrorString());
      contents->clear();
      ::CloseHandle(f);
      return -EIO;
    }
    if (len == 0)
      break;
    contents->append(buf, len);
  }
  ::CloseHandle(f);
  return 0;
#else
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    err->assign(strerror(errno));
    return -errno;
  }

#ifdef __USE_LARGEFILE64
  struct stat64 st;
  if (fstat64(fileno(f), &st) < 0) {
#else
  struct stat st;
  if (fstat(fileno(f), &st) < 0) {
#endif
    err->assign(strerror(errno));
    fclose(f);
    return -errno;
  }

  // +1 is for the resize in ManifestParser::Load
  contents->reserve(st.st_size + 1);

  char buf[64 << 10];
  size_t len;
  while (!feof(f) && (len = fread(buf, 1, sizeof(buf), f)) > 0) {
    contents->append(buf, len);
  }
  if (ferror(f)) {
    err->assign(strerror(errno));  // XXX errno?
    contents->clear();
    fclose(f);
    return -errno;
  }
  fclose(f);
  return 0;
#endif
}

void SetCloseOnExec(int fd) {
#ifndef _WIN32
  int flags = fcntl(fd, F_GETFD);
  if (flags < 0) {
    perror("fcntl(F_GETFD)");
  } else {
    if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
      perror("fcntl(F_SETFD)");
  }
#else
  HANDLE hd = (HANDLE) _get_osfhandle(fd);
  if (! SetHandleInformation(hd, HANDLE_FLAG_INHERIT, 0)) {
    fprintf(stderr, "SetHandleInformation(): %s", GetLastErrorString().c_str());
  }
#endif  // ! _WIN32
}


const char* SpellcheckStringV(const string& text,
                              const vector<const char*>& words) {
  const bool kAllowReplacements = true;
  const int kMaxValidEditDistance = 3;

  int min_distance = kMaxValidEditDistance + 1;
  const char* result = NULL;
  for (vector<const char*>::const_iterator i = words.begin();
       i != words.end(); ++i) {
    int distance = EditDistance(*i, text, kAllowReplacements,
                                kMaxValidEditDistance);
    if (distance < min_distance) {
      min_distance = distance;
      result = *i;
    }
  }
  return result;
}

const char* SpellcheckString(const char* text, ...) {
  // Note: This takes a const char* instead of a string& because using
  // va_start() with a reference parameter is undefined behavior.
  va_list ap;
  va_start(ap, text);
  vector<const char*> words;
  const char* word;
  while ((word = va_arg(ap, const char*)))
    words.push_back(word);
  va_end(ap);
  return SpellcheckStringV(text, words);
}

#ifdef _WIN32
string GetLastErrorString() {
  DWORD err = GetLastError();

  char* msg_buf;
  FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        err,
        MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT),
        (char*)&msg_buf,
        0,
        NULL);

  if (msg_buf == nullptr) {
    char fallback_msg[128] = {0};
    snprintf(fallback_msg, sizeof(fallback_msg), "GetLastError() = %lu", err);
    return fallback_msg;
  }

  string msg = msg_buf;
  LocalFree(msg_buf);
  return msg;
}

void Win32Fatal(const char* function, const char* hint) {
  if (hint) {
    Fatal("%s: %s (%s)", function, GetLastErrorString().c_str(), hint);
  } else {
    Fatal("%s: %s", function, GetLastErrorString().c_str());
  }
}
#endif

bool islatinalpha(int c) {
  // isalpha() is locale-dependent.
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

string StripAnsiEscapeCodes(const string& in) {
  string stripped;
  stripped.reserve(in.size());

  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] != '\33') {
      // Not an escape code.
      stripped.push_back(in[i]);
      continue;
    }

    // Only strip CSIs for now.
    if (i + 1 >= in.size()) break;
    if (in[i + 1] != '[') continue;  // Not a CSI.
    i += 2;

    // Skip everything up to and including the next [a-zA-Z].
    while (i < in.size() && !islatinalpha(in[i]))
      ++i;
  }
  return stripped;
}

#if defined(__linux__) || defined(__GLIBC__)
std::pair<int64_t, bool> readCount(const std::string& path) {
  std::ifstream file(path.c_str());
  if (!file.is_open())
    return std::make_pair(0, false);
  int64_t n = 0;
  file >> n;
  if (file.good())
    return std::make_pair(n, true);
  return std::make_pair(0, false);
}

struct MountPoint {
  int mountId;
  int parentId;
  StringPiece deviceId;
  StringPiece root;
  StringPiece mountPoint;
  vector<StringPiece> options;
  vector<StringPiece> optionalFields;
  StringPiece fsType;
  StringPiece mountSource;
  vector<StringPiece> superOptions;
  bool parse(const string& line) {
    vector<StringPiece> pieces = SplitStringPiece(line, ' ');
    if (pieces.size() < 10)
      return false;
    size_t optionalStart = 0;
    for (size_t i = 6; i < pieces.size(); i++) {
      if (pieces[i] == "-") {
        optionalStart = i + 1;
        break;
      }
    }
    if (optionalStart == 0)
      return false;
    if (optionalStart + 3 != pieces.size())
      return false;
    mountId = atoi(pieces[0].AsString().c_str());
    parentId = atoi(pieces[1].AsString().c_str());
    deviceId = pieces[2];
    root = pieces[3];
    mountPoint = pieces[4];
    options = SplitStringPiece(pieces[5], ',');
    optionalFields =
        vector<StringPiece>(&pieces[6], &pieces[optionalStart - 1]);
    fsType = pieces[optionalStart];
    mountSource = pieces[optionalStart + 1];
    superOptions = SplitStringPiece(pieces[optionalStart + 2], ',');
    return true;
  }
  string translate(string& path) const {
    // path must be sub dir of root
    if (path.compare(0, root.len_, root.str_, root.len_) != 0) {
      return string();
    }
    path.erase(0, root.len_);
    if (path == ".." || (path.length() > 2 && path.compare(0, 3, "../") == 0)) {
      return string();
    }
    return mountPoint.AsString() + "/" + path;
  }
};

struct CGroupSubSys {
  int id;
  string name;
  vector<string> subsystems;
  bool parse(string& line) {
    size_t first = line.find(':');
    if (first == string::npos)
      return false;
    line[first] = '\0';
    size_t second = line.find(':', first + 1);
    if (second == string::npos)
      return false;
    line[second] = '\0';
    id = atoi(line.c_str());
    name = line.substr(second + 1);
    vector<StringPiece> pieces =
        SplitStringPiece(StringPiece(line.c_str() + first + 1), ',');
    for (size_t i = 0; i < pieces.size(); i++) {
      subsystems.push_back(pieces[i].AsString());
    }
    return true;
  }
};

map<string, string> ParseMountInfo(map<string, CGroupSubSys>& subsystems) {
  map<string, string> cgroups;
  ifstream mountinfo("/proc/self/mountinfo");
  if (!mountinfo.is_open())
    return cgroups;
  while (!mountinfo.eof()) {
    string line;
    getline(mountinfo, line);
    MountPoint mp;
    if (!mp.parse(line))
      continue;
    if (mp.fsType == "cgroup") {
      for (size_t i = 0; i < mp.superOptions.size(); i++) {
        std::string opt = mp.superOptions[i].AsString();
        auto subsys = subsystems.find(opt);
        if (subsys == subsystems.end()) {
          continue;
        }
        std::string newPath = mp.translate(subsys->second.name);
        if (!newPath.empty()) {
          cgroups.emplace(opt, newPath);
        }
      }
    } else if (mp.fsType == "cgroup2") {
      // Find cgroup2 entry in format "0::/path/to/cgroup"
      auto subsys = std::find_if(subsystems.begin(), subsystems.end(),
                                 [](const auto& sys) {
                                   return sys.first == "" && sys.second.id == 0;
                                 });
      if (subsys == subsystems.end()) {
        continue;
      }
      std::string path = mp.mountPoint.AsString();
      if (subsys->second.name != "/") {
        // Append the relative path for the cgroup to the mount point
        path.append(subsys->second.name);
      }
      cgroups.emplace("cgroup2", path);
    }
  }
  return cgroups;
}

map<string, CGroupSubSys> ParseSelfCGroup() {
  map<string, CGroupSubSys> cgroups;
  ifstream cgroup("/proc/self/cgroup");
  if (!cgroup.is_open())
    return cgroups;
  string line;
  while (!cgroup.eof()) {
    getline(cgroup, line);
    CGroupSubSys subsys;
    if (!subsys.parse(line))
      continue;
    for (size_t i = 0; i < subsys.subsystems.size(); i++) {
      cgroups.insert(make_pair(subsys.subsystems[i], subsys));
    }
  }
  return cgroups;
}

int ParseCgroupV1(std::string& path) {
  std::pair<int64_t, bool> quota = readCount(path + "/cpu.cfs_quota_us");
  if (!quota.second || quota.first == -1)
    return -1;
  std::pair<int64_t, bool> period = readCount(path + "/cpu.cfs_period_us");
  if (!period.second)
    return -1;
  if (period.first == 0)
    return -1;
  return quota.first / period.first;
}

int ParseCgroupV2(std::string& path) {
  // Read CPU quota from cgroup v2
  std::ifstream cpu_max(path + "/cpu.max");
  if (!cpu_max.is_open()) {
    return -1;
  }
  std::string max_line;
  if (!std::getline(cpu_max, max_line) || max_line.empty()) {
    return -1;
  }
  // Format is "quota period" or "max period"
  size_t space_pos = max_line.find(' ');
  if (space_pos == string::npos) {
    return -1;
  }
  std::string quota_str = max_line.substr(0, space_pos);
  std::string period_str = max_line.substr(space_pos + 1);
  if (quota_str == "max") {
    return -1;  // No CPU limit set
  }
  // Convert quota string to integer
  char* quota_end = nullptr;
  errno = 0;
  int64_t quota = strtoll(quota_str.c_str(), &quota_end, 10);
  // Check for conversion errors
  if (errno == ERANGE || quota_end == quota_str.c_str() || *quota_end != '\0' ||
      quota <= 0) {
    return -1;
  }
  // Convert period string to integer
  char* period_end = nullptr;
  errno = 0;
  int64_t period = strtoll(period_str.c_str(), &period_end, 10);
  // Check for conversion errors
  if (errno == ERANGE || period_end == period_str.c_str() ||
      *period_end != '\0' || period <= 0) {
    return -1;
  }
  return quota / period;
}

int ParseCPUFromCGroup() {
  auto subsystems = ParseSelfCGroup();
  auto cgroups = ParseMountInfo(subsystems);

  // Prefer cgroup v2 if both v1 and v2 should be present
  const auto cgroup2 = cgroups.find("cgroup2");
  if (cgroup2 != cgroups.end()) {
    return ParseCgroupV2(cgroup2->second);
  }

  const auto cpu = cgroups.find("cpu");
  if (cpu != cgroups.end()) {
    return ParseCgroupV1(cpu->second);
  }
  return -1;
}
#endif

int GetProcessorCount() {
#ifdef _WIN32
  DWORD cpuCount = 0;
#ifndef _WIN64
  // Need to use GetLogicalProcessorInformationEx to get real core count on
  // machines with >64 cores. See https://stackoverflow.com/a/31209344/21475
  DWORD len = 0;
  if (!GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len)
        && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
    std::vector<char> buf(len);
    int cores = 0;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore,
          reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
            buf.data()), &len)) {
      for (DWORD i = 0; i < len; ) {
        auto info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
            buf.data() + i);
        if (info->Relationship == RelationProcessorCore &&
            info->Processor.GroupCount == 1) {
          for (KAFFINITY core_mask = info->Processor.GroupMask[0].Mask;
               core_mask; core_mask >>= 1) {
            cores += (core_mask & 1);
          }
        }
        i += info->Size;
      }
      if (cores != 0) {
        cpuCount = cores;
      }
    }
  }
#endif
  if (cpuCount == 0) {
    cpuCount = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  }
  JOBOBJECT_CPU_RATE_CONTROL_INFORMATION info;
  // reference:
  // https://docs.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_cpu_rate_control_information
  if (QueryInformationJobObject(NULL, JobObjectCpuRateControlInformation, &info,
                                sizeof(info), NULL)) {
    if (info.ControlFlags & (JOB_OBJECT_CPU_RATE_CONTROL_ENABLE |
                             JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP)) {
      return cpuCount * info.CpuRate / 10000;
    }
  }
  return cpuCount;
#else
  int cgroupCount = -1;
  int schedCount = -1;
#if defined(__linux__) || defined(__GLIBC__)
  cgroupCount = ParseCPUFromCGroup();
#endif
  // The number of exposed processors might not represent the actual number of
  // processors threads can run on. This happens when a CPU set limitation is
  // active, see https://github.com/ninja-build/ninja/issues/1278
#if defined(__FreeBSD__)
  cpuset_t mask;
  CPU_ZERO(&mask);
  if (cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, sizeof(mask),
    &mask) == 0) {
    return CPU_COUNT(&mask);
  }
#elif defined(CPU_COUNT)
  cpu_set_t set;
  if (sched_getaffinity(getpid(), sizeof(set), &set) == 0) {
    schedCount = CPU_COUNT(&set);
  }
#endif
  if (cgroupCount >= 0 && schedCount >= 0) return std::min(cgroupCount, schedCount);
  if (cgroupCount < 0 && schedCount < 0)
    return static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
  return std::max(cgroupCount, schedCount);
#endif
}

#if defined(_WIN32) || defined(__CYGWIN__)
static double CalculateProcessorLoad(uint64_t idle_ticks, uint64_t total_ticks)
{
  static uint64_t previous_idle_ticks = 0;
  static uint64_t previous_total_ticks = 0;
  static double previous_load = -0.0;

  uint64_t idle_ticks_since_last_time = idle_ticks - previous_idle_ticks;
  uint64_t total_ticks_since_last_time = total_ticks - previous_total_ticks;

  bool first_call = (previous_total_ticks == 0);
  bool ticks_not_updated_since_last_call = (total_ticks_since_last_time == 0);

  double load;
  if (first_call || ticks_not_updated_since_last_call) {
    load = previous_load;
  } else {
    // Calculate load.
    double idle_to_total_ratio =
        ((double)idle_ticks_since_last_time) / total_ticks_since_last_time;
    double load_since_last_call = 1.0 - idle_to_total_ratio;

    // Filter/smooth result when possible.
    if(previous_load > 0) {
      load = 0.9 * previous_load + 0.1 * load_since_last_call;
    } else {
      load = load_since_last_call;
    }
  }

  previous_load = load;
  previous_total_ticks = total_ticks;
  previous_idle_ticks = idle_ticks;

  return load;
}

static uint64_t FileTimeToTickCount(const FILETIME & ft)
{
  uint64_t high = (((uint64_t)(ft.dwHighDateTime)) << 32);
  uint64_t low  = ft.dwLowDateTime;
  return (high | low);
}

double GetLoadAverage() {
  FILETIME idle_time, kernel_time, user_time;
  BOOL get_system_time_succeeded =
      GetSystemTimes(&idle_time, &kernel_time, &user_time);

  double posix_compatible_load;
  if (get_system_time_succeeded) {
    uint64_t idle_ticks = FileTimeToTickCount(idle_time);

    // kernel_time from GetSystemTimes already includes idle_time.
    uint64_t total_ticks =
        FileTimeToTickCount(kernel_time) + FileTimeToTickCount(user_time);

    double processor_load = CalculateProcessorLoad(idle_ticks, total_ticks);
    posix_compatible_load = processor_load * GetProcessorCount();

  } else {
    posix_compatible_load = -0.0;
  }

  return posix_compatible_load;
}
#elif defined(__PASE__)
double GetLoadAverage() {
  return -0.0f;
}
#elif defined(_AIX)
double GetLoadAverage() {
  perfstat_cpu_total_t cpu_stats;
  if (perfstat_cpu_total(NULL, &cpu_stats, sizeof(cpu_stats), 1) < 0) {
    return -0.0f;
  }

  // Calculation taken from comment in libperfstats.h
  return double(cpu_stats.loadavg[0]) / double(1 << SBITS);
}
#elif defined(__UCLIBC__) || (defined(__BIONIC__) && __ANDROID_API__ < 29)
double GetLoadAverage() {
  struct sysinfo si;
  if (sysinfo(&si) != 0)
    return -0.0f;
  return 1.0 / (1 << SI_LOAD_SHIFT) * si.loads[0];
}
#elif defined(__HAIKU__)
double GetLoadAverage() {
    return -0.0f;
}
#else
double GetLoadAverage() {
  double loadavg[3] = { 0.0f, 0.0f, 0.0f };
  if (getloadavg(loadavg, 3) < 0) {
    // Maybe we should return an error here or the availability of
    // getloadavg(3) should be checked when ninja is configured.
    return -0.0f;
  }
  return loadavg[0];
}
#endif // _WIN32

std::string GetWorkingDirectory() {
  std::string ret;
  char* success = NULL;
  do {
    ret.resize(ret.size() + 1024);
    errno = 0;
    success = getcwd(&ret[0], ret.size());
  } while (!success && errno == ERANGE);
  if (!success) {
    Fatal("cannot determine working directory: %s", strerror(errno));
  }
  ret.resize(strlen(&ret[0]));
  return ret;
}

bool Truncate(const string& path, size_t size, string* err) {
#ifdef _WIN32
  int fh = _sopen(path.c_str(), _O_RDWR | _O_CREAT, _SH_DENYNO,
                  _S_IREAD | _S_IWRITE);
  int success = _chsize(fh, size);
  _close(fh);
#else
  int success = truncate(path.c_str(), size);
#endif
  // Both truncate() and _chsize() return 0 on success and set errno and return
  // -1 on failure.
  if (success < 0) {
    *err = strerror(errno);
    return false;
  }
  return true;
}

bool ReplaceContent(const string& file_dst, const string& new_content,
                    string* err) {
#ifndef _WIN32
  struct stat old_file;
  bool found_uid_gid = true;

  if (stat(file_dst.c_str(), &old_file) < 0) {
    found_uid_gid = false;
  }
#endif

  if (platformAwareUnlink(file_dst.c_str()) < 0) {
    *err = strerror(errno);
    return false;
  }

  if (rename(new_content.c_str(), file_dst.c_str()) < 0) {
    *err = strerror(errno);
    return false;
  }

#ifndef _WIN32
  // apply uid and gid again so we always stay as the uid and gid of the first
  // ninja invocation that created the file at file_dst
  if (found_uid_gid) {
    if (chown(file_dst.c_str(), old_file.st_uid, old_file.st_gid)) {
      Warning("Reapplying previous uid and gid failed with: %s uid: %d gid: %d",
              strerror(errno), old_file.st_uid, old_file.st_gid);
    }
  }
#endif

  return true;
}

int platformAwareUnlink(const char* filename) {
	#ifdef _WIN32
		return _unlink(filename);
	#else
		return unlink(filename);
	#endif
}
