// Copyright 2012 Google Inc. All Rights Reserved.
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

#include <stdio.h>
#include <string.h>
#include <intrin.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <numeric>
#include <vector>

#include "string_piece.h"
#include "metrics.h"
#include "util.h"

const std::string kPaths[] = {
  // Relative
  "third_party/WebKit/Source/WebCore/"
  "platform/leveldb/LevelDBWriteBatch.cpp",
  // Absolute
  "/third_party/WebKit/Source/WebCore/"
  "platform/leveldb/LevelDBWriteBatch.cpp",
  // Leading ../
  "../../third_party/WebKit/Source/WebCore/"
  "platform/leveldb/LevelDBWriteBatch.cpp",
  // Leading ./ (causing us actually copy bytes)
  "./third_party/WebKit/Source/WebCore/"
  "platform/leveldb/LevelDBWriteBatch.cpp",
  // ../ in the middle
  "third_party/WebKit/Source/WebCore/"
  "platform/leveldb/UnnecessarySubDirectory/../LevelDBWriteBatch.cpp",
  // Empty directories
  "third_party//WebKit///Source/WebCore/"
  "platform/leveldb/LevelDBWriteBatch.cpp",

#if 1
  // Handle the same paths again but with backslashes
  // Relative
  "third_party\\WebKit\\Source\\WebCore\\"
  "platform\\leveldb\\LevelDBWriteBatch.cpp",
  // Absolute
  "\\third_party\\WebKit\\Source\\WebCore\\"
  "platform\\leveldb\\LevelDBWriteBatch.cpp",
  // Leading ..\ dir
  "..\\..\\third_party\\WebKit\\Source\\WebCore\\"
  "platform\\leveldb\\LevelDBWriteBatch.cpp",
  // Leading .\ (causing us actually copy bytes)
  ".\\third_party\\WebKit\\Source\\WebCore\\"
  "platform\\leveldb\\LevelDBWriteBatch.cpp",
  // ..\ in the middle
  "third_party\\WebKit\\Source\\WebCore\\"
  "platform\\leveldb\\UnnecessarySubDirectory\\..\\LevelDBWriteBatch.cpp",
  // Empty directories
  "third_party\\\\WebKit\\\\\\Source\\WebCore\\"
  "platform\\leveldb\\LevelDBWriteBatch.cpp",

  // Mixture of slashes
  "third_party\\WebKit/Source\\WebCore\\"
  "platform\\leveldb/LevelDBWriteBatch.cpp",
#endif
};

static bool IsPathSeparator(char c) {
#ifdef _WIN32
  return c == '/' || c == '\\';
#else
  return c == '/';
#endif
}

#if 1
using SWAR = std::uint64_t;
#else
struct SWAR {
  std::uint64_t data;
};

SWAR operator|(SWAR l, SWAR r) {
  return SWAR{ l.data | r.data };
}

SWAR operator&(SWAR l, SWAR r) {
  return SWAR{ l.data & r.data };
}

SWAR operator^(SWAR l, SWAR r) {
  return SWAR{ l.data ^ r.data };
}

SWAR operator~(SWAR l) {
  return SWAR{ ~l.data };
}

SWAR operator==(SWAR lhs, SWAR rhs) {
  const SWAR msb{ 0x80'80'80'80'80'80'80'80ull };
  const SWAR lsb{ 0x01'01'01'01'01'01'01'01ull };
  const SWAR zero_if_equal = lhs ^ rhs;
  const SWAR A = zero_if_equal | msb;
  const SWAR B{ A.data - lsb.data };
  const SWAR C = zero_if_equal | B;
  const SWAR D = ~C;
  const SWAR FINAL = D & msb;
  return FINAL;
}
#endif

template <typename T>
__forceinline std::uint64_t combine(const SWAR* x, T&& thing)
{
  return (thing(x[0]) << 0ull) | (thing(x[1]) << 8ull) | (thing(x[2]) << 16ull) |
         (thing(x[3]) << 24ull) | (thing(x[4]) << 32ull) |
         (thing(x[5]) << 40ull) | (thing(x[6]) << 48ull) |
         (thing(x[7]) << 56ull);
}

__forceinline std::uint8_t get_equal_bytes(SWAR lhs,
                                           SWAR rhs) {
  const SWAR equal = lhs == rhs;
#if 0
  return (equal.data * 0x02'04'08'10'20'40'81ull) >> 56;
#else
  return (equal * 0x02'04'08'10'20'40'81ull) >> 56;
#endif
}

__forceinline std::uint64_t backslash_mask_u64(SWAR v) {
  constexpr SWAR BS{ 0x5C5C5C5C5C5C5C5CULL };
  return get_equal_bytes(v, BS);
}

__forceinline std::uint64_t slash_mask_u64(SWAR v) {
  constexpr SWAR SLASH{ 0x2F2F2F2F2F2F2F2FULL };
#if 1
  return get_equal_bytes(v, SLASH);
#else
  // 1) slash bytes -> 0
  v ^= SLASH;

  // 2) Per-byte: reduce "is nonzero" into the low bit of each byte.
  // After these steps, bit0 of each byte is 1 iff that byte was nonzero.
  v |= (v >> 4) & 0x0F0F0F0F0F0F0F0FULL;
  v |= (v >> 2) & 0x0303030303030303ULL;
  v |= (v >> 1) & 0x0101010101010101ULL;
  v &= 0x0101010101010101ULL;

  // Now: v has 1 in each byte if (byte != 0). Flip to get 1 for (byte ==
  // 0)
  // => slash.
  v ^= 0x0101010101010101ULL;

  // 3) Gather the low bit from each byte into bits [0..7]
  return (std::uint8_t)((v * 0x0102040810204080ULL) >> 56);
#endif
}

__forceinline std::uint64_t dot_mask_u64(SWAR v) {
  constexpr SWAR DOT{ 0x2e2e2e2e2e2e2e2eULL };
#if 1
  return get_equal_bytes(v, DOT);
#else
  v ^= DOT;

  // 2) Per-byte: reduce "is nonzero" into the low bit of each byte.
  // After these steps, bit0 of each byte is 1 iff that byte was nonzero.
  v |= (v >> 4) & 0x0F0F0F0F0F0F0F0FULL;
  v |= (v >> 2) & 0x0303030303030303ULL;
  v |= (v >> 1) & 0x0101010101010101ULL;
  v &= 0x0101010101010101ULL;

  // Now: v has 1 in each byte if (byte != 0). Flip to get 1 for (byte ==
  // 0)
  // => slash.
  v ^= 0x0101010101010101ULL;

  // 3) Gather the low bit from each byte into bits [0..7]
  return (std::uint8_t)((v * 0x0102040810204080ULL) >> 56);
#endif
}

void disambiguation2(char* path, std::size_t* len, std::uint64_t* slash_bits) {
  // Disambiguate between overloads of CanonicalizePath
  CanonicalizePath2(path, len, slash_bits);
}

/*
What's the plan?

We need to remove
  1. Empty paths
  2. Current paths (.)
  3. Parent paths (..)

(And keep the initial slashes for absolute paths).

Assume that every path ends with a slash to make things easier

  1. Empty paths

  This can be detected by by doing equality with / and then shifting and bitwise and to detect

  2. Current paths

  This can be detected by doing equality with . and slashes, then shifting and bitwise to detect

  3. Parent paths

  This can be detected the same way, but we need to be able to remove the previous path as well,
  in this case we could just do a non SWAR solution - but that feels inelegant.

  So we can look at the slashes we intend *not* to remove and we need to see if we can "fill in the
  gaps" between any parent paths and the slashes we intend to keep.

  "abc/defghi/../zy/"
  "00010000001001001" slashes (A)
  "00000000000001000" parent path (B)
  "00010000000000000" slashes to remove (C)

  "00000000000001111" B | (B - 1)
  "00011111111111111" C | (C - 1) 

  "00011111111110000" to remove (WANT) = C & ~B

  */

void CheatyPath(char* path, std::size_t* len, std::uint64_t* slash_bit) {
  if (*len == 0) {
    return;
  }
 
  const std::size_t chunks = *len / 64;
  //assert(chunks * 8 == *len); // FIX later
  char* dst = path;
  const char* src = path;
  for (std::size_t i = 0; i < chunks; ++i) {
#if 1
    SWAR temp[8];
    std::memcpy(&temp, src, sizeof(temp));
#else
    const SWAR* temp = reinterpret_cast<const SWAR*>(src);
#endif
    // "//a/b/c/" -> 11010101
    const std::uint64_t slash_bits =
#ifdef _WIN32
        combine(temp, backslash_mask_u64) &
#endif
        combine(temp, slash_mask_u64);
    const std::uint64_t dot_bits = combine(temp, dot_mask_u64);
    
    // Look at empty paths
    const std::uint64_t empty_paths_to_remove = slash_bits & (slash_bits >> 1u);

    // Look at current path /./
    const std::uint64_t current_path_indicator =
        (slash_bits << 2u) & (dot_bits << 1u) & slash_bits;
    const std::uint64_t current_path_to_remove =
        current_path_indicator | (current_path_indicator >> 1u);

    // Look at parent path
    
    const std::uint64_t parent_path_indicator =
        (slash_bits << 3u) & (dot_bits << 2u) & (dot_bits << 1u) & slash_bits;
    // Loop while parent_path_indicator is not 0;
    const std::uint64_t kept_slashes = slash_bits & ~empty_paths_to_remove &
                                       ~current_path_to_remove &
                                       ~(parent_path_indicator << 3u);
    // Looks at the lowest slash and sets everything to the left of it
    const std::uint64_t slashes_to_the_left = [&](){
      const std::uint64_t low = kept_slashes & -kept_slashes;
      return kept_slashes | ~(low - 1);
    }();
    const std::uint64_t dirs_to_remove =
        ~slashes_to_the_left & ~(parent_path_indicator - 1);

    std::uint64_t to_remove =
        empty_paths_to_remove | current_path_to_remove;
    // tODO: Add dirs_to_remove
    std::int64_t start = 0;
    while (to_remove) {
      unsigned long last_index_to_keep;
      _BitScanForward64(&last_index_to_keep, to_remove);
      const std::int64_t end = last_index_to_keep + 1;
      std::memcpy(dst, reinterpret_cast<const char*>(&temp) + start,
                  end - start);
      dst += end - start;
      to_remove &= ~(1ull << last_index_to_keep);
      start = end + 1;
    }
    if (dst != src) {
      std::memcpy(dst, reinterpret_cast<const char*>(&temp) + start,
                  sizeof(temp) - start);
    }
    dst += sizeof(temp) - start;
    src += sizeof(temp);
  }
 
  *len = dst - path;
}

void CanonicalizePathOriginal(char* path, size_t* len, uint64_t* slash_bits) {
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

const int kNumRepetitions = 1000000;
const int kNumRepeats = 10;

template <typename CANONICALIZE_PATH>
void runBenchmarks(CANONICALIZE_PATH&& f, const char* name,
                   std::string& pathCopies) {
  printf("%s\n", name);

  int sum_of_min = 0;
  int sum_of_max = 0;
  double sum_of_avg = 0.0;

  for (const std::string& path : kPaths) {
    printf("%s\n", path.c_str());
    int min = std::numeric_limits<int>::max();
    int max = std::numeric_limits<int>::min();
    double total = 0;

    for (int j = 0; j < kNumRepeats; ++j) {
      auto out = pathCopies.begin();
      for (int i = 0; i < kNumRepetitions; ++i) {
        out = std::copy(path.begin(), path.end(), out);
      }

      const std::int64_t start = GetTimeMillis();
      for (int i = 0; i < kNumRepetitions; ++i) {
        std::uint64_t slash_bits;
        std::size_t len = path.size();
        f(&pathCopies[i * len], &len, &slash_bits);
      }

      const int delta = static_cast<int>(GetTimeMillis() - start);
      min = std::min(delta, min);
      max = std::max(delta, max);
      total += delta;
    }

    const double avg = total / kNumRepeats;
    printf("  min %dms  max %dms  avg %.1fms\n", min, max, avg);

    sum_of_min += min;
    sum_of_max += max;
    sum_of_avg += avg;
  }

  printf("AVERAGE:\n  min %dms  max %dms  avg %.1fms\n",
         static_cast<int>(sum_of_min / std::size(kPaths)),
         static_cast<int>(sum_of_max / std::size(kPaths)),
         sum_of_avg / std::size(kPaths));
}

int main() {
  //std::string test =
  //    "th/./_party//WebKit///Source/WebCore/platform/leveldb/ssfdhs.cpp";
  std::string test =
      "0/00000000000000000000000000000000000000000000000000000000000000";
  auto x = test.size();
  CheatyPath(&test[0], &x, nullptr);
  test.resize(x);
  std::size_t max_size = 0;
  for (const std::string& path : kPaths) {
    max_size = std::max(max_size, path.size());
  }

  std::string pathCopies;
  pathCopies.resize(kNumRepetitions * max_size);
  runBenchmarks(disambiguation2, "CanonicalizePath2", pathCopies);
  runBenchmarks(CanonicalizePathOriginal, "CanonicalizePathOriginal", pathCopies);
  //runBenchmarks(CheatyPath, "CheatyPath", pathCopies);

  // add additional implementations here for comparison
}
