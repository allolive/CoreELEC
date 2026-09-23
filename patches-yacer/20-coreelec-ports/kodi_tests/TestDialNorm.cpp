/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Dialogue-normalisation defeat in passthrough. The patch rewrites dialnorm
 *  and then has to fix the frame's own checksum, and its commit message
 *  records a shipped regression from getting exactly that wrong - a wrong
 *  checksum on every frame, and receivers going silent on DD+.
 *
 *  So the checksum is not imitated here. libavutil's crc.c is compiled into
 *  this binary from the tarball the build already pins, which is also what
 *  lets the test produce input frames that are well formed.
 */

#include "cores/AudioEngine/Utils/AEStreamInfo.h"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

extern "C"
{
#include <libavutil/crc.h>

// crc.c reports a bad table request through libavutil's logger, which lives
// in a translation unit this binary has no reason to compile. One definition
// here is the whole of it.
void av_log(void*, int, const char*, ...)
{
}
}

namespace
{
constexpr uint8_t DIALNORM_NONE = 31; // 0 dB of attenuation asked of the receiver

// An E-AC-3 frame: sync word, then the bit-stream information the parser
// reads, then padding, then the checksum that makes the whole frame verify.
std::vector<uint8_t> Eac3Frame(uint8_t dialnorm, uint8_t strmtyp = 0, size_t bytes = 64)
{
  std::vector<uint8_t> frame(bytes, 0);
  const unsigned frmsiz = static_cast<unsigned>(bytes / 2 - 1); // in 16-bit words, minus one
  frame[0] = 0x0B;
  frame[1] = 0x77;
  frame[2] = static_cast<uint8_t>((strmtyp << 6) | ((frmsiz >> 8) & 0x07));
  frame[3] = static_cast<uint8_t>(frmsiz & 0xFF);
  // fscod 0 (48 kHz), numblkscod 3 (6 blocks), acmod 2 (stereo), lfeon 0
  frame[4] = static_cast<uint8_t>((0 << 6) | (3 << 4) | (2 << 1) | 0);
  // bsid 16 marks this as E-AC-3; dialnorm's top three bits follow it.
  frame[5] = static_cast<uint8_t>((16 << 3) | ((dialnorm >> 2) & 0x07));
  frame[6] = static_cast<uint8_t>((dialnorm & 0x03) << 6);

  // The frame verifies when the checksum over everything after the sync word
  // comes out zero, which is what storing it byte-swapped achieves.
  const AVCRC* table = av_crc_get_table(AV_CRC_16_ANSI);
  const uint32_t crc = av_crc(table, 0, frame.data() + 2, frame.size() - 4);
  frame[frame.size() - 2] = static_cast<uint8_t>(crc & 0xFF);
  frame[frame.size() - 1] = static_cast<uint8_t>(crc >> 8);
  return frame;
}

bool Verifies(const std::vector<uint8_t>& frame)
{
  const AVCRC* table = av_crc_get_table(AV_CRC_16_ANSI);
  return av_crc(table, 0, frame.data() + 2, frame.size() - 2) == 0;
}

uint8_t DialNormOf(const std::vector<uint8_t>& frame)
{
  return static_cast<uint8_t>(((frame[5] & 0x07) << 2) | (frame[6] >> 6));
}
} // namespace

// Proves the frame builder agrees with the production checksum before any
// test leans on it.
TEST(DialNorm, TheFramesThisSuiteBuildsVerify)
{
  EXPECT_TRUE(Verifies(Eac3Frame(20)));
  EXPECT_TRUE(Verifies(Eac3Frame(DIALNORM_NONE)));

  auto damaged = Eac3Frame(20);
  damaged[8] ^= 0xFF;
  EXPECT_FALSE(Verifies(damaged));
}

TEST(DialNorm, ReadsBackTheDialnormItWroteIn)
{
  EXPECT_EQ(DialNormOf(Eac3Frame(20)), 20);
  EXPECT_EQ(DialNormOf(Eac3Frame(1)), 1);
  EXPECT_EQ(DialNormOf(Eac3Frame(DIALNORM_NONE)), DIALNORM_NONE);
}

namespace
{
// Run a frame through the parser and hand back what it would pass to the sink.
std::vector<uint8_t> Passthrough(const std::vector<uint8_t>& frame, bool defeat)
{
  // A frame is only confirmed by the sync word of the one after it, so a
  // single frame on its own is never emitted. Feed two, as a stream does.
  std::vector<uint8_t> stream(frame);
  stream.insert(stream.end(), frame.begin(), frame.end());

  CAEStreamParser parser;
  parser.SetDefeatAC3DialNorm(defeat);
  uint8_t* out = nullptr;
  unsigned int outSize = 0;
  unsigned int offset = 0;
  for (int guard = 0; guard < 16 && outSize == 0 && offset < stream.size(); ++guard)
  {
    const int used = parser.AddData(stream.data() + offset,
                                    static_cast<unsigned int>(stream.size() - offset),
                                    &out, &outSize);
    if (used <= 0)
      break;
    offset += static_cast<unsigned int>(used);
  }
  if (out == nullptr || outSize == 0)
    return {};
  return std::vector<uint8_t>(out, out + outSize);
}
} // namespace

TEST(DialNorm, RewritesDialnormAndLeavesTheFrameVerifiable)
{
  const auto out = Passthrough(Eac3Frame(20), true);
  ASSERT_FALSE(out.empty()) << "the parser produced no frame";
  EXPECT_EQ(DialNormOf(out), DIALNORM_NONE);
  // The regression this patch records was a checksum computed over the wrong
  // bytes, which silenced receivers. The rewritten frame must still verify.
  EXPECT_TRUE(Verifies(out));
}

TEST(DialNorm, LeavesTheFrameAloneWhenTheDefeatIsOff)
{
  const auto out = Passthrough(Eac3Frame(20), false);
  ASSERT_FALSE(out.empty());
  EXPECT_EQ(DialNormOf(out), 20);
  EXPECT_TRUE(Verifies(out));
}

TEST(DialNorm, LeavesADependentSubstreamAlone)
{
  // Stream type 1 carries no bit-stream information of its own to rewrite.
  const auto out = Passthrough(Eac3Frame(20, 1), true);
  if (!out.empty())
  {
    EXPECT_EQ(DialNormOf(out), 20);
  }
}

TEST(DialNorm, LeavesAFrameThatFailsItsOwnChecksumAlone)
{
  auto damaged = Eac3Frame(20);
  damaged[8] ^= 0xFF; // corrupt a byte the checksum covers
  const auto out = Passthrough(damaged, true);
  if (!out.empty())
  {
    EXPECT_EQ(DialNormOf(out), 20) << "rewrote a frame it could not trust";
  }
}

TEST(DialNorm, LeavesAFrameAlreadyAtFullLevelAlone)
{
  const auto out = Passthrough(Eac3Frame(DIALNORM_NONE), true);
  ASSERT_FALSE(out.empty());
  EXPECT_EQ(DialNormOf(out), DIALNORM_NONE);
  EXPECT_TRUE(Verifies(out));
}
