/*
 *  Copyright (C) 2026 Team CoreELEC
 *  This file is part of CoreELEC - https://coreelec.org
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

// TrueHD out of the real passthrough codec, parser and MAT packer must start
// on the first major-sync access unit the parser accepted, and every frame
// after it must follow on that timeline: no early first stamp for the minor
// units dropped ahead of the major, no jump on the second frame.
//
// The access units are parser-valid framing (the major header's CRC and the
// minor header's parity are real) with a synthetic payload: they are not
// decodable TrueHD, and nothing here is played or heard.

#include "DVDCodecs/DVDCodecs.h"
#include "cores/VideoPlayer/DVDCodecs/Audio/DVDAudioCodecPassthrough.h"
#include "cores/AudioEngine/Utils/AEUtil.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

extern "C"
{
#include <libavutil/crc.h>

void av_log(void*, int, const char*, ...)
{
}
}

// The codec builds its layout from RAW channels and never guesses one.
CAEChannelInfo CAEUtil::GuessChLayout(const unsigned int channels)
{
  ADD_FAILURE() << "the codec reached channel-layout guessing for " << channels;
  return {};
}

namespace
{
constexpr double START = 127000000.0; // the first packet's PTS, us
constexpr uint16_t COUNTER = 65000; // its timing counter, so the 16-bit counter wraps

std::vector<uint8_t> Unit(uint16_t time, unsigned int rate, bool major)
{
  std::vector<uint8_t> frame(192, 0x5C);
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
    frame[11] = 1; // two encoded channels
    frame[20] = 0x10; // one substream
    frame[34] = 0xF1;
    frame[35] = 0xEA;
    const uint16_t output = static_cast<uint16_t>(time + (40U << (rate & 7)));
    frame[36] = static_cast<uint8_t>(output >> 8);
    frame[37] = static_cast<uint8_t>(output);
    AVCRC table[1024];
    av_crc_init(table, 0, 16, 0x2D, sizeof(table));
    uint16_t crc = static_cast<uint16_t>(av_crc(table, 0, frame.data() + 4, 24));
    crc ^= static_cast<uint16_t>((frame[29] << 8) | frame[28]);
    frame[30] = static_cast<uint8_t>(crc);
    frame[31] = static_cast<uint8_t>(crc >> 8);
  }
  else
  {
    uint8_t check = 0;
    for (unsigned int i = 0; i < 6; ++i)
      check ^= frame[i];
    frame[5] ^= ((check >> 4) ^ check ^ 0x0F) & 0x0F;
  }
  return frame;
}

struct Case
{
  unsigned int rate = 0; // the major sync's audio_sampling_frequency
  unsigned int skipped = 0; // minor units ahead of the first major
  unsigned int grouped = 1; // units per demux packet
  bool split = false; // the first major across two packets, the second without a PTS
  bool ptsOnMajorsOnly = false; // after the first major, only majors carry a PTS
  bool raw = false;
  double start = START;
};

struct Played
{
  double firstAccepted = DVD_NOPTS_VALUE; // PTS of the first major the parser kept
  double span = 0; // one MAT frame, or one 24-unit RAW group
  std::vector<double> pts;
};

// Feeds a case to a codec; the first major the parser actually keeps is found
// by a second, independent instance of the real parser, from the unit's counter.
void Feed(CDVDAudioCodecPassthrough& codec, const Case& c, Played& run)
{
  const unsigned int samples = 40U << (c.rate & 7);
  const unsigned int sampleRate = ((c.rate & 8) ? 44100U : 48000U) << (c.rate & 7);
  const double unit = static_cast<double>(samples) / sampleRate * DVD_TIME_BASE;
  run.span = 24 * unit;

  CAEStreamParser provenance;
  uint8_t* parsed = nullptr;
  unsigned int parsedSize = 0;
  const auto feed = [&](std::vector<uint8_t> input, double pts)
  {
    provenance.AddData(input.data(), static_cast<unsigned int>(input.size()), &parsed, &parsedSize);
    if (run.firstAccepted == DVD_NOPTS_VALUE && parsedSize >= 8 && parsed[4] == 0xF8 &&
        parsed[5] == 0x72 && parsed[6] == 0x6F && parsed[7] == 0xBA)
    {
      const uint16_t delta = static_cast<uint16_t>(((parsed[2] << 8) | parsed[3]) - COUNTER);
      ASSERT_EQ(delta % samples, 0U);
      run.firstAccepted = c.start + (delta / samples) * unit;
    }
    DemuxPacket packet;
    packet.pData = input.data();
    packet.iSize = static_cast<int>(input.size());
    packet.pts = pts;
    packet.dts = pts;
    ASSERT_TRUE(codec.AddData(packet));
    for (unsigned int drain = 0; drain < 16; ++drain)
    {
      DVDAudioFrame frame{};
      codec.GetData(frame);
      if (frame.nb_frames == 0)
        break;
      ASSERT_TRUE(frame.passthrough);
      ASSERT_EQ(frame.format.m_streamInfo.m_type, CAEStreamInfo::STREAM_TYPE_TRUEHD);
      run.pts.push_back(frame.pts);
    }
  };

  for (unsigned int i = 0; i < c.skipped + 240;)
  {
    const unsigned int first = i;
    std::vector<uint8_t> packet;
    for (unsigned int count = 0; count < c.grouped && i < c.skipped + 240; ++count, ++i)
    {
      const bool major = i >= c.skipped && (i - c.skipped) % 24 == 0;
      auto bytes = Unit(static_cast<uint16_t>(COUNTER + i * samples), c.rate, major);
      if (c.split && i == c.skipped)
      {
        feed(std::vector<uint8_t>(bytes.begin(), bytes.begin() + 17), c.start + i * unit);
        feed(std::vector<uint8_t>(bytes.begin() + 17, bytes.end()), DVD_NOPTS_VALUE);
      }
      else
        packet.insert(packet.end(), bytes.begin(), bytes.end());
    }
    if (packet.empty())
      continue;
    // The dropped minors keep theirs: they are what used to label the first frame.
    const bool stamped = !c.ptsOnMajorsOnly || first < c.skipped ||
                         (first - c.skipped) % 24 == 0;
    feed(std::move(packet), stamped ? c.start + first * unit : DVD_NOPTS_VALUE);
  }
  delete[] parsed;
}

Played Play(const Case& c)
{
  CProcessInfo process;
  process.raw = c.raw;
  CDVDAudioCodecPassthrough codec(process, CAEStreamInfo::STREAM_TYPE_TRUEHD);
  CDVDStreamInfo hints;
  CDVDCodecOptions options;
  EXPECT_TRUE(codec.Open(hints, options));
  codec.Reset();
  Played run;
  Feed(codec, c, run);
  return run;
}

void ExpectTimeline(const Played& run)
{
  ASSERT_NE(run.firstAccepted, DVD_NOPTS_VALUE) << "the parser kept no major";
  ASSERT_GE(run.pts.size(), 3U) << "too few frames out";
  for (size_t k = 0; k < run.pts.size(); ++k)
  {
    ASSERT_NE(run.pts[k], DVD_NOPTS_VALUE) << "frame " << k << " has no PTS";
    EXPECT_NEAR(run.pts[k], run.firstAccepted + k * run.span, 1.0) << "frame " << k;
  }
}

class TrueHDRates : public testing::TestWithParam<std::tuple<unsigned int, unsigned int>>
{
};

TEST_P(TrueHDRates, MATFramesFollowTheFirstAcceptedMajor)
{
  Case c;
  std::tie(c.rate, c.skipped) = GetParam();
  ExpectTimeline(Play(c));
}

// 48, 96, 192 kHz and 44.1, 88.2, 176.4 kHz; already on a major, 12 and 96
// minor units dropped ahead of it (10 and 80 ms at 48 kHz).
INSTANTIATE_TEST_SUITE_P(AllRates,
                         TrueHDRates,
                         testing::Combine(testing::Values(0U, 1U, 2U, 8U, 9U, 10U),
                                          testing::Values(0U, 12U, 96U)));

TEST(TrueHDTimeline, FirstMajorSplitAcrossPackets)
{
  Case c;
  c.skipped = 12;
  c.split = true;
  ExpectTimeline(Play(c));
}

// The parser drops a major it saw only in part and keeps the next one, 20 ms on.
TEST(TrueHDTimeline, FirstMajorSplitWithNoMinorsAhead)
{
  Case c;
  c.split = true;
  const Played run = Play(c);
  EXPECT_NEAR(run.firstAccepted, START + 20000.0, 1.0);
  ExpectTimeline(run);
}

// The parser holds the units it has not handed out yet and the codec keeps
// what the parser would not take: a unit's packet is found through both. (One
// unit per packet is what FFmpeg's TrueHD parser hands Kodi; the parser hands
// out one unit per call, so many per packet would wait for later packets.)
TEST(TrueHDTimeline, SeveralUnitsPerPacket)
{
  Case c;
  c.skipped = 12;
  c.grouped = 3;
  ExpectTimeline(Play(c));
}

TEST(TrueHDTimeline, PacketsWithoutPts)
{
  Case c;
  c.skipped = 12;
  c.ptsOnMajorsOnly = true;
  ExpectTimeline(Play(c));
}

TEST(TrueHDTimeline, RawGroupsFollowTheFirstAcceptedMajor)
{
  Case c;
  c.skipped = 12;
  c.raw = true;
  ExpectTimeline(Play(c));
}

// A seek: the codec is reset and the stream starts again elsewhere, again
// behind minor units. The second timeline starts on its own first major.
TEST(TrueHDTimeline, ResetStartsANewTimeline)
{
  CProcessInfo process;
  CDVDAudioCodecPassthrough codec(process, CAEStreamInfo::STREAM_TYPE_TRUEHD);
  CDVDStreamInfo hints;
  CDVDCodecOptions options;
  ASSERT_TRUE(codec.Open(hints, options));

  Case before;
  before.skipped = 12;
  Played first;
  Feed(codec, before, first);
  ExpectTimeline(first);

  codec.Reset();
  Case after;
  after.skipped = 96;
  after.start = 300000000.0;
  Played second;
  Feed(codec, after, second);
  ExpectTimeline(second);
}
} // namespace
