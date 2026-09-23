/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  A guard: upstream behaviour our parsers rest on, with no patch of ours
 *  behind it. CBitstreamReader bounds every read against the length it was
 *  given and answers 0 past the end. Our SEI and HDR10+ parsers read counts
 *  out of the stream and then read that many fields, and it is this bound -
 *  not anything we wrote - that keeps a malformed stream inside the buffer.
 *  If upstream ever relaxes it, those parsers become overreads and nothing
 *  else in the suite would say so.
 */

#include "utils/BitstreamReader.h"

#include <cstdint>
#include <numeric>
#include <vector>

#include <gtest/gtest.h>

namespace
{
constexpr size_t LENGTH = 8;

// 0x10, 0x11, ... so every byte is distinguishable in an assertion.
std::vector<uint8_t> Buffer()
{
  std::vector<uint8_t> bytes(LENGTH);
  std::iota(bytes.begin(), bytes.end(), static_cast<uint8_t>(0x10));
  return bytes;
}
} // namespace

TEST(BitstreamReaderContract, ReportsTheWholeBufferBeforeAnythingIsRead)
{
  auto bytes = Buffer();
  CBitstreamReader reader(bytes.data(), static_cast<int>(bytes.size()));
  EXPECT_EQ(reader.Position(), 0u);
  EXPECT_EQ(reader.AvailableBits(), LENGTH * 8);
}

TEST(BitstreamReaderContract, ReturnsTheBytesItWasGiven)
{
  auto bytes = Buffer();
  CBitstreamReader reader(bytes.data(), static_cast<int>(bytes.size()));
  EXPECT_EQ(reader.ReadBits(8), 0x10u);
  EXPECT_EQ(reader.ReadBits(16), 0x1112u);
  EXPECT_EQ(reader.Position(), 24u);
  EXPECT_EQ(reader.AvailableBits(), LENGTH * 8 - 24);
}

// The last byte is the one an off-by-one bound stops returning, and a parser
// reading a final field would silently see zero instead of the data.
TEST(BitstreamReaderContract, ReadsTheLastByteOfTheBuffer)
{
  auto bytes = Buffer();
  CBitstreamReader reader(bytes.data(), static_cast<int>(bytes.size()));
  reader.SkipBits(static_cast<int>((LENGTH - 1) * 8));
  EXPECT_EQ(reader.ReadBits(8), 0x17u);
  EXPECT_EQ(reader.AvailableBits(), 0u);
}

TEST(BitstreamReaderContract, ReadsAFieldEndingExactlyAtTheBuffersEnd)
{
  auto bytes = Buffer();
  CBitstreamReader reader(bytes.data(), static_cast<int>(bytes.size()));
  reader.SkipBits(static_cast<int>((LENGTH - 2) * 8));
  EXPECT_EQ(reader.ReadBits(16), 0x1617u);
}

// The property the parsers depend on: a read that would cross the end returns
// zero rather than the bytes that happen to follow the buffer.
TEST(BitstreamReaderContract, AnswersZeroInsteadOfReadingPastTheEnd)
{
  auto bytes = Buffer();
  CBitstreamReader reader(bytes.data(), static_cast<int>(bytes.size()));
  reader.SkipBits(static_cast<int>((LENGTH - 1) * 8));
  // One byte remains; asking for two must not reach the byte after it.
  EXPECT_EQ(reader.ReadBits(16), 0u);
}

TEST(BitstreamReaderContract, KeepsCountingThePositionAfterAnOverread)
{
  auto bytes = Buffer();
  CBitstreamReader reader(bytes.data(), static_cast<int>(bytes.size()));
  reader.SkipBits(static_cast<int>(LENGTH * 8));
  const auto before = reader.Position();
  EXPECT_EQ(reader.ReadBits(8), 0u);
  // Callers detect an overrun by comparing the position against the length
  // they passed in; a position that stops advancing makes that check dead.
  EXPECT_EQ(reader.Position(), before + 8);
}
