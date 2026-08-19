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
// tests and conversions over 64-bit lanes, thin wrappers over the bit-scan,
// popcount and pext intrinsics, and an iterator over runs of equal bits.
// Everything is defined inline because these are small, hot functions that
// must inline into their callers (see canon_perftest).

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

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

#ifdef _WIN32
#pragma intrinsic(_BitScanForward64,_BitScanReverse64)
#endif

inline bool bit_scan_forward64(unsigned long* index, std::uint64_t x) {
#ifdef _WIN32
 return _BitScanForward64(index, x) != 0;
#else
    if (x == 0) return false;
    *index = __builtin_ctzll(x);
    return true;
#endif
}

inline bool bit_scan_reverse64(unsigned long* index, std::uint64_t x) {
#ifdef _WIN32
 return _BitScanReverse64(index, x) != 0;
#else
  if (x == 0) return false;
  *index = 63 - __builtin_clzll(x);
  return true;
#endif
}

inline int popcnt64(std::uint64_t x) {
#ifdef _WIN32
  return __popcnt64(x);
#else
  return __builtin_popcountll(x);
#endif
}

inline std::uint64_t equal(std::uint64_t lhs, std::uint8_t c) {
  const std::uint64_t rhs = 0x0101010101010101ull * c;
  const std::uint64_t zero_if_equal = lhs ^ rhs;
  return ~(zero_if_equal | ((zero_if_equal | msb) - lsb));
}

// Set MSB for each char to 1 if it equals 'c'
inline std::array<std::uint64_t, 8> msb_equal(const std::uint64_t* lhs,
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
#undef EQUAL_TEST
  return result;
}

NEEDS_BMI2_INTRINSICS 
inline std::uint64_t compress(const std::uint64_t *v) {
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

inline void get_slashdot(const std::uint64_t* lhs,
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

inline void get_slashdot(const std::array<std::uint64_t, 8>& lhs,
                  std::uint64_t* forwardslashes, std::uint64_t* dots) {
  get_slashdot(lhs.data(), forwardslashes, dots);
}

inline std::uint64_t convert_backslashes_msb(const std::uint64_t text, const std::uint64_t backslashes_msb) {
  return text ^ (backslashes_msb >> 7) * 0x73ull;
}

inline void convert_backslashes(std::uint64_t* text,
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
#undef EQUAL_TEST
}

const std::uint64_t all_dots = lsb * '.';
const std::uint64_t mask = ~lsb;

NEEDS_BMI2_INTRINSICS 
inline std::uint64_t get_slashdot_indicator(const std::uint64_t word) {
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

// Iterates over the runs of equal bits in a 64-bit value, yielding the
// length and value of each run.  Only the first \a limit bits are considered,
// which lets us stop at the real end of a partial final chunk instead of
// walking into the synthetic padding bits.
struct Biterator {
  std::uint64_t bits_;
  unsigned limit_;
  unsigned consumed_ = 0;
  bool done_ = false;
  CountAndValue value_{};

  explicit Biterator(std::uint64_t bits, unsigned limit)
      : bits_(bits), limit_(limit) {
    // The value of the first run is simply the value of the first bit.
    value_.value = (bits_ & 1ull) != 0;
    read_next();
  }

  void read_next() {
    if (consumed_ == limit_) {
      done_ = true;
      return;
    }

    const std::uint64_t remaining_bits = bits_ >> consumed_;

    const std::uint64_t value_mask =
        0ull - static_cast<std::uint64_t>(value_.value);
    const std::uint64_t changed = remaining_bits ^ value_mask;

    unsigned long first_changed_bit;
    const bool found_changed_bit =
        bit_scan_forward64(&first_changed_bit, changed);

    unsigned count = found_changed_bit
                         ? static_cast<unsigned>(first_changed_bit)
                         : (64 - consumed_);

    // Don't let a run extend past the requested limit (e.g. into the
    // padding bits of the final, partial chunk).
    if (count > limit_ - consumed_)
      count = limit_ - consumed_;

    assert(count > 0);
    value_.count = static_cast<int>(count);
    consumed_ += count;
  }

  Biterator& operator++() {
    // The next run necessarily has the opposite value.
    value_.value = !value_.value;
    read_next();
    return *this;
  }

  CountAndValue operator*() const { return value_; }
};

struct EndSentinel {};

inline bool operator==(const Biterator& lhs, EndSentinel) {
  return lhs.done_;
}

inline bool operator!=(const Biterator& lhs, EndSentinel rhs) {
  return !(lhs == rhs);
}

struct BitRange {
  std::uint64_t bits_;
  unsigned limit_;
  BitRange(std::uint64_t bits, unsigned limit) : bits_(bits), limit_(limit) {}

  Biterator begin() { return Biterator(bits_, limit_); }

  EndSentinel end() { return EndSentinel(); }
};

NEEDS_BMI2_INTRINSICS 
inline std::uint64_t get_slashdot_indicator(const std::uint64_t* buffer, std::size_t count) {
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

NEEDS_BMI2_INTRINSICS
inline std::uint64_t convert_backslashes(std::uint64_t* buffer, std::size_t count) {
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
inline std::uint64_t get_lsb_indicator(const std::uint64_t* buffer,
                                std::size_t count) {
  std::uint64_t lsb_indicator = 0;
  for (int i = 0; i < count; ++i) {
    const std::uint64_t bits = _pext_u64((buffer[i] & lsb) << 7, msb);
    lsb_indicator |= bits << (i * 8);
  }
  return lsb_indicator;
}

NEEDS_BMI2_INTRINSICS 
inline std::uint64_t get_backslash_indicator(std::uint64_t* buffer,
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
  return backslash_indicator;
}

#endif  // NINJA_SWAR_H_
