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
  "platform/leveldb//LevelDBWriteBatch.cpp",

#if _WIN32
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
  "platform\\leveldb\\\\LevelDBWriteBatch.cpp",

  // Mixture of slashes
  "third_party\\WebKit/Source\\WebCore\\"
  "platform\\leveldb/LevelDBWriteBatch.cpp",
#endif
};

void disambiguation(char* path, std::size_t* len, std::uint64_t* slash_bits) {
  // Disambiguate between overloads of CanonicalizePath
  CanonicalizePath(path, len, slash_bits);
}

const int kNumRepetitions = 2000000;
const int kNumRepeats = 5;

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
  std::size_t max_size = 0;
  for (const std::string& path : kPaths) {
    max_size = std::max(max_size, path.size());
  }

  std::string pathCopies;
  pathCopies.resize(kNumRepetitions * max_size);
  runBenchmarks(disambiguation, "CanonicalizePath",
                pathCopies);
  // add additional implementations here for comparison
}
