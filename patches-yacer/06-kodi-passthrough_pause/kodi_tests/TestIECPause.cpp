/*
 *  Copyright (C) 2026 Team CoreELEC
 *  This file is part of CoreELEC - https://coreelec.org
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

// The pause carrier is cached, so most of what can go wrong is the cache
// answering for the wrong thing: a different policy, rate, codec or byte order.
// Every expectation below reads the packed bytes rather than asking the packer
// what it thinks it produced.

#include "cores/AudioEngine/Utils/AEBitstreamPacker.h"
#include "cores/AudioEngine/Utils/AEStreamInfo.h"
#include "cores/AudioEngine/Utils/AEUtil.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

// Generating a pause carrier never guesses a channel layout - every case
// here builds its CAEStreamInfo outright. Defining it keeps AEChannelInfo
// linkable and fails the test loudly if some path ever does reach it.
CAEChannelInfo CAEUtil::GuessChLayout(const unsigned int channels)
{
  ADD_FAILURE() << "pause packing reached channel-layout guessing for " << channels
                << " channels";
  return {};
}

namespace
{
CAEStreamInfo Info(CAEStreamInfo::DataType type, unsigned int rate = 48000)
{
  CAEStreamInfo info{};
  info.m_type = type;
  info.m_sampleRate = rate;
  info.m_channels = 8;
  info.m_dtsPeriod = 2048;
  info.m_repeat = 1;
  return info;
}

uint16_t Word(const uint8_t* data)
{
  uint16_t word;
  std::memcpy(&word, data, sizeof(word));
  return word;
}

std::vector<uint8_t> Bytes(CAEBitstreamPacker& packer)
{
  return {packer.GetBuffer(), packer.GetBuffer() + packer.GetSize()};
}

void SwapWords(CAEBitstreamPacker& packer)
{
  ASSERT_EQ(0u, packer.GetSize() % 2) << "packed output is not whole 16-bit words";
  for (unsigned int i = 0; i < packer.GetSize(); i += 2)
    std::swap(packer.GetBuffer()[i], packer.GetBuffer()[i + 1]);
}

// Explicit carrier expectations, not values read back from the packer's own
// format helpers - those are the thing under test.
void ExpectPause(CAEBitstreamPacker& packer, unsigned int millis, unsigned int rate,
                 unsigned int channels, unsigned int repetition, unsigned int encodedRate,
                 bool bursts)
{
  const unsigned int frameBytes = channels * 2;
  const unsigned int periodBytes = repetition * frameBytes;
  const uint64_t wantedPeriods = uint64_t{millis} * rate / (1000 * repetition);
  const uint64_t periods = std::min(wantedPeriods, uint64_t{MAX_IEC61937_PACKET / periodBytes});

  ASSERT_EQ(periods * periodBytes, packer.GetSize()) << "incorrect generated carrier byte count";
  ASSERT_EQ(0u, packer.GetSize() % frameBytes) << "output is not whole carrier frames";
  const auto bytes = Bytes(packer);

  if (!bursts)
  {
    EXPECT_TRUE(std::all_of(bytes.begin(), bytes.end(), [](uint8_t b) { return b == 0; }))
        << "zero-output policy leaked a preamble or program payload";
    return;
  }

  for (uint64_t period = 0; period < periods; ++period)
  {
    const uint8_t* burst = bytes.data() + period * periodBytes;
    ASSERT_EQ(0xf872, Word(burst)) << "missing IEC preamble in period " << period;
    ASSERT_EQ(0x4e1f, Word(burst + 2)) << "missing IEC preamble in period " << period;
    ASSERT_EQ(3, Word(burst + 4)) << "incorrect IEC pause type in period " << period;
    ASSERT_EQ(32, Word(burst + 6)) << "incorrect IEC pause length in period " << period;
    // The IEC routine writes the encoded-sample gap into the first burst only,
    // after repeating the empty ones. That existing layout is preserved.
    const uint16_t gap =
        period == 0 ? static_cast<uint16_t>(uint64_t{encodedRate} * millis / 1000) : 0;
    ASSERT_EQ(gap, Word(burst + 8)) << "encoded-rate pause gap changed in period " << period;
    ASSERT_TRUE(std::all_of(burst + 10, burst + periodBytes, [](uint8_t b) { return b == 0; }))
        << "IEC pause payload leaked previous program bytes in period " << period;
  }
}

TEST(IECPause, SwitchingPolicyAtTheSameDurationRegeneratesTheCarrier)
{
  auto info = Info(CAEStreamInfo::STREAM_TYPE_TRUEHD);
  CAEBitstreamPacker packer;
  ASSERT_TRUE(packer.PackPause(info, 20, false)) << "initial zeros were incorrectly cached";
  ExpectPause(packer, 20, 192000, 8, 4, 48000, false);

  ASSERT_TRUE(packer.PackPause(info, 20, true)) << "zeros-to-IEC transition reused stale zeros";
  ExpectPause(packer, 20, 192000, 8, 4, 48000, true);

  EXPECT_FALSE(packer.PackPause(info, 20, true)) << "identical request did not reuse its bytes";

  ASSERT_TRUE(packer.PackPause(info, 20, false)) << "IEC-to-zeros transition reused IEC data";
  ExpectPause(packer, 20, 192000, 8, 4, 48000, false);
}

TEST(IECPause, GoingFromBurstsToZerosIsNotTreatedAsACacheHit)
{
  auto info = Info(CAEStreamInfo::STREAM_TYPE_DTSHD_MA);
  CAEBitstreamPacker packer;
  ASSERT_TRUE(packer.PackPause(info, 10, true)) << "initial IEC request failed";
  ASSERT_TRUE(packer.PackPause(info, 10, false)) << "IEC-to-zeros policy change was ignored";
  ExpectPause(packer, 10, 192000, 8, 3, 48000, false);
}

TEST(IECPause, AChangeOfEncodedOrCarrierRateInvalidatesTheCache)
{
  auto info = Info(CAEStreamInfo::STREAM_TYPE_TRUEHD);
  CAEBitstreamPacker packer;
  packer.PackPause(info, 20, true);

  info.m_sampleRate = 96000; // Same carrier, different encoded gap.
  ASSERT_TRUE(packer.PackPause(info, 20, true)) << "encoded-rate change did not invalidate";
  ExpectPause(packer, 20, 192000, 8, 4, 96000, true);

  info.m_sampleRate = 44100;
  ASSERT_TRUE(packer.PackPause(info, 20, true)) << "44.1-family carrier change did not invalidate";
  ExpectPause(packer, 20, 176400, 8, 4, 44100, true);

  info.m_sampleRate = 88200;
  ASSERT_TRUE(packer.PackPause(info, 20, true)) << "encoded rate changed inside 44.1-family cache";
  ExpectPause(packer, 20, 176400, 8, 4, 88200, true);
}

TEST(IECPause, AChangeOfCodecInvalidatesRepetitionAndChannelLayout)
{
  auto info = Info(CAEStreamInfo::STREAM_TYPE_DTSHD);
  CAEBitstreamPacker packer;
  packer.PackPause(info, 10, true);
  ExpectPause(packer, 10, 192000, 2, 3, 48000, true);

  info.m_type = CAEStreamInfo::STREAM_TYPE_EAC3; // Same carrier size, different repetition.
  ASSERT_TRUE(packer.PackPause(info, 10, true)) << "repetition change reused the previous layout";
  ExpectPause(packer, 10, 192000, 2, 4, 48000, true);

  info.m_type = CAEStreamInfo::STREAM_TYPE_DTSHD_MA;
  ASSERT_TRUE(packer.PackPause(info, 10, true)) << "channel-count change reused previous layout";
  ExpectPause(packer, 10, 192000, 8, 3, 48000, true);
}

struct PauseCase
{
  CAEStreamInfo::DataType type;
  unsigned int encodedRate, millis, carrierRate, channels, repetition;
};

class IECPauseFormats : public testing::TestWithParam<PauseCase>
{
};

TEST_P(IECPauseFormats, AFreshFormatPacksTheCarrierItsLayoutCallsFor)
{
  const auto& item = GetParam();
  auto info = Info(item.type, item.encodedRate);
  CAEBitstreamPacker packer;
  ASSERT_TRUE(packer.PackPause(info, item.millis, true)) << "fresh format did not generate output";
  ExpectPause(packer, item.millis, item.carrierRate, item.channels, item.repetition,
              item.encodedRate, true);
}

INSTANTIATE_TEST_SUITE_P(
    EveryPassthroughLayout,
    IECPauseFormats,
    testing::Values(PauseCase{CAEStreamInfo::STREAM_TYPE_TRUEHD, 48000, 20, 192000, 8, 4},
                    PauseCase{CAEStreamInfo::STREAM_TYPE_TRUEHD, 44100, 20, 176400, 8, 4},
                    PauseCase{CAEStreamInfo::STREAM_TYPE_TRUEHD, 44100, 21, 176400, 8, 4},
                    PauseCase{CAEStreamInfo::STREAM_TYPE_DTSHD_MA, 48000, 10, 192000, 8, 3},
                    PauseCase{CAEStreamInfo::STREAM_TYPE_DTSHD, 48000, 10, 192000, 2, 3},
                    PauseCase{CAEStreamInfo::STREAM_TYPE_AC3, 48000, 20, 48000, 2, 3},
                    PauseCase{CAEStreamInfo::STREAM_TYPE_AC3, 44100, 20, 44100, 2, 3},
                    PauseCase{CAEStreamInfo::STREAM_TYPE_EAC3, 48000, 20, 192000, 2, 4},
                    PauseCase{CAEStreamInfo::STREAM_TYPE_TRUEHD, 48000, 100, 192000, 8, 4}));

TEST(IECPause, TheExistingPacketCapStillBoundsALongRequest)
{
  // The 61,440-byte cap is deliberately unchanged: a 100 ms request at
  // 192 kHz and eight channels yields 20 ms of carrier, not 100 ms.
  auto info = Info(CAEStreamInfo::STREAM_TYPE_TRUEHD);
  CAEBitstreamPacker packer;
  packer.PackPause(info, 100, true);
  const double actualMs = 1000.0 * packer.GetSize() / (192000.0 * 8 * 2);
  EXPECT_NEAR(20.0, actualMs, 1e-9) << "carrier cap/duration unexpectedly changed";
}

TEST(IECPause, PackingAProgramOrResettingInvalidatesTheCachedPause)
{
  auto info = Info(CAEStreamInfo::STREAM_TYPE_AC3);
  CAEBitstreamPacker packer;
  packer.PackPause(info, 20, true);

  // Synthetic packer input, not a decoder-readiness fixture.
  std::vector<uint8_t> payload(128, 0xa5);
  packer.Pack(info, payload.data(), static_cast<int>(payload.size()));
  ASSERT_NE(3, Word(packer.GetBuffer() + 4)) << "program packet was not packed";

  ASSERT_TRUE(packer.PackPause(info, 20, true)) << "Pack did not invalidate the pause cache";
  ExpectPause(packer, 20, 48000, 2, 3, 48000, true);

  packer.Reset();
  EXPECT_EQ(0u, packer.GetSize()) << "Reset retained output bytes";
  ASSERT_TRUE(packer.PackPause(info, 20, true)) << "Reset did not invalidate the pause cache";
  ExpectPause(packer, 20, 48000, 2, 3, 48000, true);

  packer.Pack(info, payload.data(), static_cast<int>(payload.size()));
  ASSERT_TRUE(packer.PackPause(info, 20, false)) << "Pack-to-zeros failed";
  ExpectPause(packer, 20, 48000, 2, 3, 48000, false);
}

TEST(IECPause, AZeroDurationRequestIsNotAValidCacheKey)
{
  auto info = Info(CAEStreamInfo::STREAM_TYPE_AC3);
  CAEBitstreamPacker packer;
  packer.PackPause(info, 20, true);
  packer.Reset();
  EXPECT_TRUE(packer.PackPause(info, 0, true)) << "zero duration was mistaken for a valid key";
  EXPECT_EQ(0u, packer.GetSize()) << "zero-duration request exposed stale payload";
}

TEST(IECPause, ACacheHitDoesNotAskTheSinkToSwapBytesTwice)
{
  auto info = Info(CAEStreamInfo::STREAM_TYPE_TRUEHD);
  CAEBitstreamPacker packer;
  packer.PackPause(info, 20, true);
  const auto nativeBytes = Bytes(packer);

  SwapWords(packer); // ActiveAESink swaps the packer's owned buffer in place.
  const auto swappedBytes = Bytes(packer);
  ASSERT_NE(nativeBytes, swappedBytes) << "fixture does not exercise byte swapping";

  EXPECT_FALSE(packer.PackPause(info, 20, true)) << "cache hit asked for a second swap";
  EXPECT_EQ(swappedBytes, Bytes(packer)) << "cache hit rewrote the already-swapped buffer";

  ASSERT_TRUE(packer.PackPause(info, 20, false)) << "swapped IEC-to-zero transition failed";
  ExpectPause(packer, 20, 192000, 8, 4, 48000, false);

  SwapWords(packer);
  EXPECT_FALSE(packer.PackPause(info, 20, false)) << "cached zeros unexpectedly regenerated";
  ASSERT_TRUE(packer.PackPause(info, 20, true)) << "zero-to-IEC did not request one new swap";
  EXPECT_EQ(nativeBytes, Bytes(packer)) << "regeneration did not restore native byte order";

  SwapWords(packer);
  EXPECT_EQ(swappedBytes, Bytes(packer)) << "regenerated output does not match sink byte order";
}

TEST(IECPause, AFormatWithNoPauseLayoutYieldsNothingRatherThanStaleProgram)
{
  auto info = Info(CAEStreamInfo::STREAM_TYPE_AC3);
  CAEBitstreamPacker packer;
  std::vector<uint8_t> payload(128, 0x5a);
  packer.Pack(info, payload.data(), static_cast<int>(payload.size()));

  info.m_type = CAEStreamInfo::STREAM_TYPE_MLP; // No supported IEC pause layout.
  EXPECT_TRUE(packer.PackPause(info, 20, true)) << "unsupported format looked reusable";
  EXPECT_EQ(0u, packer.GetSize()) << "unsupported format exposed previous program payload";
  EXPECT_TRUE(packer.PackPause(info, 20, true)) << "unsupported format became a cache entry";
  EXPECT_EQ(0u, packer.GetSize()) << "repeated unsupported request produced output";

  info.m_type = CAEStreamInfo::STREAM_TYPE_AC3;
  ASSERT_TRUE(packer.PackPause(info, 20, true)) << "supported format did not recover";
  ExpectPause(packer, 20, 48000, 2, 3, 48000, true);
}
} // namespace
