/*
 *  Copyright (C) 2026 Team CoreELEC
 *  This file is part of CoreELEC - https://coreelec.org
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

// Reset has to leave the packer indistinguishable from a new one, so each case
// drives a reset packer and a fresh packer with the same frames and requires
// the bytes to match. Anything the old stream left behind shows up as a
// difference rather than as a judgement call about what "clean" means.

#include "cores/AudioEngine/Utils/PackerMAT.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace
{
constexpr size_t MAT_BYTES = 61440;
constexpr uint8_t OLD_PAYLOAD = 0xA7;
constexpr uint8_t NEW_PAYLOAD = 0x5C;

// Synthetic parser-output fixtures exercise MAT framing, not TrueHD decoding.
// A major unit provides rate bits and a first-substream restart timing header;
// payload bytes and checksums are intentionally not a decodable audio sample.
std::vector<uint8_t> Frame(uint16_t time, unsigned int rate, bool major, uint8_t payload)
{
  std::vector<uint8_t> frame(192, payload);
  frame[0] = 0;
  frame[1] = static_cast<uint8_t>(frame.size() / 2);
  frame[2] = static_cast<uint8_t>(time >> 8);
  frame[3] = static_cast<uint8_t>(time);
  if (major)
  {
    std::fill(frame.begin() + 4, frame.begin() + 38, 0);
    frame[4] = 0xF8;
    frame[5] = 0x72;
    frame[6] = 0x6F;
    frame[7] = 0xBA;
    frame[8] = static_cast<uint8_t>(rate << 4);
    frame[20] = 0x10; // One substream, no major-sync extension.
    frame[34] = 0xF1; // Block/restart flags followed by restart sync 0x31EA.
    frame[35] = 0xEA;
    const uint16_t outputTime = static_cast<uint16_t>(time + (40U << (rate & 7)));
    frame[36] = static_cast<uint8_t>(outputTime >> 8);
    frame[37] = static_cast<uint8_t>(outputTime);
  }
  return frame;
}

bool Pack(CPackerMAT& packer, const std::vector<uint8_t>& frame)
{
  return packer.PackTrueHD(frame.data(), static_cast<int>(frame.size()));
}

bool HasOldPayload(const std::vector<uint8_t>& packet)
{
  std::array<uint8_t, 32> marker{};
  marker.fill(OLD_PAYLOAD);
  return std::search(packet.begin(), packet.end(), marker.begin(), marker.end()) != packet.end();
}

bool FeedOld(CPackerMAT& packer, unsigned int count, unsigned int rate = 0)
{
  bool output = false;
  for (unsigned int i = 0; i < count; ++i)
  {
    const uint16_t time = static_cast<uint16_t>(65000U + i * (40U << (rate & 7)));
    output = Pack(packer, Frame(time, rate, i % 24 == 0, OLD_PAYLOAD)) || output;
  }
  return output;
}

// The whole point of the patch: after a reset, this packer and a new one must
// answer identically for every frame.
void ExpectIndistinguishableFromFresh(CPackerMAT& reset, unsigned int rate = 0)
{
  CPackerMAT fresh;
  unsigned int packets = 0;
  for (unsigned int i = 0; i < 80; ++i)
  {
    const uint16_t time = static_cast<uint16_t>(i * (40U << (rate & 7)));
    const auto frame = Frame(time, rate, i % 24 == 0, NEW_PAYLOAD);
    ASSERT_EQ(Pack(fresh, frame), Pack(reset, frame))
        << "reset changed output availability at frame " << i;
    while (true)
    {
      const auto actual = reset.GetOutputFrame();
      const auto expected = fresh.GetOutputFrame();
      ASSERT_EQ(expected, actual) << "post-reset MAT bytes differ from a fresh packer";
      if (actual.empty())
        break;
      ASSERT_EQ(MAT_BYTES, actual.size()) << "incomplete MAT packet returned";
      ASSERT_FALSE(HasOldPayload(actual)) << "pre-reset payload leaked into new output";
      ++packets;
    }
  }
  ASSERT_GE(packets, 3u) << "fixture did not produce enough complete MAT packets to compare";
}

TEST(MATReset, ResettingAnUntouchedPackerProducesNothingAndChangesNothing)
{
  CPackerMAT packer;
  packer.Reset();
  packer.Reset();
  EXPECT_TRUE(packer.GetOutputFrame().empty()) << "empty reset created output";
  ExpectIndistinguishableFromFresh(packer);
}

TEST(MATReset, APartlyFilledPacketIsDroppedAndContinuationUnitsAreRefused)
{
  CPackerMAT packer;
  ASSERT_FALSE(FeedOld(packer, 10)) << "partial fixture unexpectedly completed a MAT packet";
  packer.Reset();
  ASSERT_TRUE(packer.GetOutputFrame().empty()) << "partial packet survived reset";

  // Without a fresh major sync there is nothing to anchor to, so every
  // continuation unit has to be refused rather than appended to stale bytes.
  for (unsigned int i = 0; i < 40; ++i)
  {
    ASSERT_FALSE(Pack(packer, Frame(static_cast<uint16_t>(i * 40), 0, false, OLD_PAYLOAD)))
        << "reset accepted a continuation unit before a new major sync, at " << i;
    ASSERT_TRUE(packer.GetOutputFrame().empty()) << "continuation units emitted stale MAT bytes";
  }
  ExpectIndistinguishableFromFresh(packer);
}

TEST(MATReset, CompletedOutputStillQueuedIsDiscarded)
{
  CPackerMAT packer;
  ASSERT_TRUE(FeedOld(packer, 80)) << "fixture did not queue completed output";
  packer.Reset();
  EXPECT_TRUE(packer.GetOutputFrame().empty()) << "queued completed output survived reset";
  ExpectIndistinguishableFromFresh(packer);
}

TEST(MATReset, OutputAlreadyHandedToTheCallerIsLeftAlone)
{
  CPackerMAT packer;
  ASSERT_TRUE(FeedOld(packer, 80)) << "fixture did not queue completed output";
  const auto delivered = packer.GetOutputFrame();
  ASSERT_EQ(MAT_BYTES, delivered.size()) << "old-output fixture invalid";
  ASSERT_TRUE(HasOldPayload(delivered)) << "old-output fixture invalid";

  packer.Reset();
  EXPECT_TRUE(packer.GetOutputFrame().empty()) << "the rest of the queue survived reset";
  EXPECT_TRUE(HasOldPayload(delivered)) << "reset reached into output it had already handed over";
  ExpectIndistinguishableFromFresh(packer);
}

// Rates 0-2 and 8-10 are the two families the rate field selects between; a
// reset has to cross from any of them to any other.
class MATResetRates : public testing::TestWithParam<std::pair<unsigned int, unsigned int>>
{
};

TEST_P(MATResetRates, ResetCrossesARateChangeCleanly)
{
  const auto [before, after] = GetParam();
  CPackerMAT packer;
  ASSERT_TRUE(FeedOld(packer, 60, before)) << "rate fixture did not queue output";
  packer.Reset();
  ASSERT_TRUE(packer.GetOutputFrame().empty()) << "old-rate queue survived reset";
  ExpectIndistinguishableFromFresh(packer, after);
}

INSTANTIATE_TEST_SUITE_P(EveryRateTransition,
                         MATResetRates,
                         testing::Values(std::pair{0u, 1u},
                                         std::pair{1u, 2u},
                                         std::pair{2u, 8u},
                                         std::pair{8u, 9u},
                                         std::pair{9u, 10u},
                                         std::pair{10u, 0u}));
} // namespace
