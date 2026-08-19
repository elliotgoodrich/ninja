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

#ifndef NINJA_SWAR_H_
#define NINJA_SWAR_H_

// Generic SWAR (SIMD within a register) utilities: word-parallel character
// tests over 64-bit lanes, and a thin wrapper over the popcount intrinsic.
// Everything is defined inline because these are small, hot functions that
// must inline into their callers (see canon_perftest).

#include <cstddef>
#include <cstdint>
#include <cstring>

#ifdef _WIN32
#include <intrin.h>
#endif
#include <immintrin.h>

#if defined(__linux__)
#define NEEDS_BMI2_INTRINSICS __attribute__((target("bmi2")))
#else
#define NEEDS_BMI2_INTRINSICS
#endif

const std::uint64_t msb{ 0x80'80'80'80'80'80'80'80ull };
const std::uint64_t lsb{ 0x01'01'01'01'01'01'01'01ull };

inline int popcnt64(std::uint64_t x) {
#ifdef _WIN32
  return __popcnt64(x);
#else
  return __builtin_popcountll(x);
#endif
}

/// Load the \a n bytes at \a p, at most 8, into the low bytes of a word and
/// zero fill the rest.  A zero byte matches no path character, so the fill
/// never looks like content.
inline std::uint64_t load_word(const char* p, std::size_t n) {
  // The whole-word copy must keep its constant size: handing memcpy a length
  // the compiler cannot fold turns this into a real call and costs the
  // scanning loops around half their speed.
  std::uint64_t w;
  if (n >= 8) {
    std::memcpy(&w, p, 8);
  } else {
    w = 0;
    std::memcpy(&w, p, n);
  }
  return w;
}

/// The high bit of each of the \a n bytes a load_word() actually read.
inline std::uint64_t valid_bytes(std::size_t n) {
  return n >= 8 ? msb
                : (msb & ((static_cast<std::uint64_t>(1) << (8 * n)) - 1));
}

inline std::uint64_t equal(std::uint64_t lhs, std::uint8_t c) {
  const std::uint64_t rhs = 0x0101010101010101ull * c;
  const std::uint64_t zero_if_equal = lhs ^ rhs;
  return ~(zero_if_equal | ((zero_if_equal | msb) - lsb));
}

/// Set the high bit of every byte of \a w holding '/' or '.'.  They differ
/// only in bit 0, so forcing that bit on finds both with one comparison.
inline std::uint64_t separators_or_dots(std::uint64_t w) {
  return equal(w | lsb, '/') & msb;
}

/// Split the result of separators_or_dots(): bit 0 of the original byte is
/// what tells a '/' from a '.'.
inline std::uint64_t separators_of(std::uint64_t w, std::uint64_t both) {
  return both & ((w & lsb) << 7);
}

#endif  // NINJA_SWAR_H_
