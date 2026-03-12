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

void disambiguation2(char* path, std::size_t* len, std::uint64_t* slash_bits) {
  // Disambiguate between overloads of CanonicalizePath
  CanonicalizePath2(path, len, slash_bits);
}

void disambiguation(char* path, std::size_t* len, std::uint64_t* slash_bits) {
  // Disambiguate between overloads of CanonicalizePath
  CanonicalizePath(path, len, slash_bits);
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
const int kNumRepeats = 20;

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

  const std::size_t size = sizeof(kPaths) / sizeof(kPaths[0]);
  printf("AVERAGE:\n  min %dms  max %dms  avg %.1fms\n",
         static_cast<int>(sum_of_min / size),
         static_cast<int>(sum_of_max / size),
         sum_of_avg / size);
}

int main() {
  std::size_t max_size = 0;
  for (const std::string& path : kPaths) {
    max_size = std::max(max_size, path.size());
  }

  std::string pathCopies;
  pathCopies.resize(kNumRepetitions * max_size);
  runBenchmarks(disambiguation2, "CanonicalizePath2 (SWAR)", pathCopies);
  runBenchmarks(disambiguation, "CanonicalizePath (original)", pathCopies);
  //runBenchmarks(disambiguation3, "CanonicalizePath3", pathCopies);
  //runBenchmarks(CheatyPath, "CheatyPath", pathCopies);

  // add additional implementations here for comparison
}
