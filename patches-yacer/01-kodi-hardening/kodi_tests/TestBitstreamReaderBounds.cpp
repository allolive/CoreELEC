/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  This group's second patch changes CBitstreamReader::AvailableBits to
 *  saturate at zero. Upstream's subtracts unsigned, so once a read has run
 *  past the end it answers about four billion - and every "does the declared
 *  length fit" test in the SEI and HDR10+ parsers then passes. It is ours, so
 *  it is tested here rather than in tests/guards, which is for behaviour that
 *  survives our patches being retired.
 */

#include "utils/BitstreamReader.h"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace
{
constexpr int LENGTH = 8;

// Longer than the reader is told about, so running the position past the end
// leaves the pointer inside the allocation: the arithmetic under test is on
// the position, and walking a pointer out of its object would be undefined
// whatever the answer.
std::vector<uint8_t> PaddedBuffer()
{
  return std::vector<uint8_t>(128, 0xA5);
}
} // namespace

TEST(BitstreamReaderBounds, AnswersZeroOnceThePositionReachesTheEnd)
{
  auto bytes = PaddedBuffer();
  CBitstreamReader reader(bytes.data(), LENGTH);
  reader.SkipBits(LENGTH * 8);
  EXPECT_EQ(reader.AvailableBits(), 0u);
}

TEST(BitstreamReaderBounds, AnswersZeroPastTheEndInsteadOfWrapping)
{
  for (int past : {1, 7, 8, 64, 4096})
  {
    auto bytes = PaddedBuffer();
    CBitstreamReader reader(bytes.data(), LENGTH);
    reader.SkipBits(LENGTH * 8 + past);
    EXPECT_EQ(reader.AvailableBits(), 0u) << "after running " << past << " bits past the end";
  }
}

TEST(BitstreamReaderBounds, ReportsTheWholeBytesLeftWhileInsideTheBuffer)
{
  // What the SEI payload-size check compares a byte count against.
  for (int consumed = 0; consumed <= LENGTH * 8; ++consumed)
  {
    auto bytes = PaddedBuffer();
    CBitstreamReader reader(bytes.data(), LENGTH);
    reader.SkipBits(consumed);
    EXPECT_EQ(static_cast<int>(reader.AvailableBits() / 8), (LENGTH * 8 - consumed) / 8)
        << "after " << consumed << " bits";
  }
}
