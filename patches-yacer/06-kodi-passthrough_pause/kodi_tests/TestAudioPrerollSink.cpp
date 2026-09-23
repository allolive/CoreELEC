/*
 *  Copyright (C) 2026 Team CoreELEC
 *  This file is part of CoreELEC - https://coreelec.org
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

// AddPrerollPacket is the backpressure guard: it hands the sink whole
// passthrough packets or nothing at all, and says which of those happened. A
// partial write here is a partial packet on the HDMI carrier, so "wrote some
// of it" has to be a failure rather than progress.
//
// The real CAudioSinkAE is compiled in. The audio engine interfaces are pure
// virtual, so gmock supplies them. Everything else it reaches for is a
// singleton reached through a header - the service registry and the master
// clock - so those are supplied as symbols by this binary. No production code
// is changed to make any of it reachable.

#include "cores/AudioEngine/Interfaces/AE.h"
#include "cores/AudioEngine/Interfaces/AEStream.h"
#include "cores/AudioEngine/Utils/AEUtil.h"
#include "cores/VideoPlayer/AudioSinkAE.h"
#include "cores/VideoPlayer/DVDClock.h"
#include "cores/VideoPlayer/DVDCodecs/Audio/DVDAudioCodec.h"
#include "ServiceBroker.h"

#include <memory>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using testing::_;
using testing::NiceMock;
using testing::Return;

// DVDClock.h only forward-declares the video reference clock, and a clock the
// sink merely reads timestamps from never builds one. Completing it here, as
// nothing, is what stops the chain: the real class is a thread with a display
// behind it, and no translation unit in this binary sees its definition.
class CVideoReferenceClock
{
};

namespace
{
class MockStream : public IAEStream
{
public:
  MOCK_METHOD(unsigned int, GetSpace, (), (override));
  MOCK_METHOD(unsigned int, AddData,
              (const uint8_t* const* data, unsigned int offset, unsigned int frames,
               IAEStream::ExtData* extData),
              (override));
  MOCK_METHOD(double, GetDelay, (), (override));
  MOCK_METHOD(CAESyncInfo, GetSyncInfo, (), (override));
  MOCK_METHOD(bool, IsBuffering, (), (override));
  MOCK_METHOD(double, GetCacheTime, (), (override));
  MOCK_METHOD(double, GetCacheTotal, (), (override));
  MOCK_METHOD(double, GetMaxDelay, (), (override));
  MOCK_METHOD(void, Pause, (), (override));
  MOCK_METHOD(void, Resume, (), (override));
  MOCK_METHOD(void, Drain, (bool wait), (override));
  MOCK_METHOD(bool, IsDraining, (), (override));
  MOCK_METHOD(bool, IsDrained, (), (override));
  MOCK_METHOD(void, Flush, (), (override));
  MOCK_METHOD(float, GetVolume, (), (override));
  MOCK_METHOD(void, SetVolume, (float volume), (override));
  MOCK_METHOD(float, GetReplayGain, (), (override));
  MOCK_METHOD(void, SetReplayGain, (float factor), (override));
  MOCK_METHOD(float, GetAmplification, (), (override));
  MOCK_METHOD(void, SetAmplification, (float amplify), (override));
  MOCK_METHOD(void, SetFFmpegInfo,
              (int profile, enum AVMatrixEncoding matrix_encoding,
               enum AVAudioServiceType audio_service_type),
              (override));
  MOCK_METHOD(unsigned int, GetFrameSize, (), (override, const));
  MOCK_METHOD(unsigned int, GetChannelCount, (), (override, const));
  MOCK_METHOD(unsigned int, GetSampleRate, (), (override, const));
  MOCK_METHOD(enum AEDataFormat, GetDataFormat, (), (override, const));
  MOCK_METHOD(double, GetResampleRatio, (), (override));
  MOCK_METHOD(void, SetResampleRatio, (double ratio), (override));
  MOCK_METHOD(void, SetResampleMode, (int mode), (override));
  MOCK_METHOD(void, RegisterAudioCallback, (IAudioCallback * pCallback), (override));
  MOCK_METHOD(void, UnRegisterAudioCallback, (), (override));
  MOCK_METHOD(void, RegisterSlave, (IAEStream * stream), (override));
};

class MockEngine : public IAE
{
public:
  MOCK_METHOD(void, Start, (), (override));
  MOCK_METHOD(bool, Suspend, (), (override));
  MOCK_METHOD(bool, Resume, (), (override));
  MOCK_METHOD(float, GetVolume, (), (override));
  MOCK_METHOD(void, SetVolume, (const float volume), (override));
  MOCK_METHOD(void, SetMute, (const bool enabled), (override));
  MOCK_METHOD(bool, IsMuted, (), (override));
  MOCK_METHOD(SoundPtr, MakeSound, (const std::string& file), (override));
  MOCK_METHOD(void, EnumerateOutputDevices, (AEDeviceList & devices, bool passthrough), (override));
  MOCK_METHOD(bool, FreeStream, (IAEStream * stream, bool finish), (override));
  MOCK_METHOD(void, FreeSound, (IAESound * sound), (override));
  MOCK_METHOD(StreamPtr, MakeStream,
              (AEAudioFormat & audioFormat, unsigned int options, IAEClockCallback* clock),
              (override));
  MOCK_METHOD(bool, SupportsRaw, (AEAudioFormat & format), (override));
  MOCK_METHOD(bool, UsesDtsCoreFallback, (), (override));
};

IAE* g_engine = nullptr;
} // namespace

// The engine the sink will find.
IAE* CServiceBroker::GetActiveAE()
{
  return g_engine;
}

// The master clock. Submitting a packet reads a timestamp from it and nothing
// more, so these answer constants rather than running a clock.
CDVDClock::CDVDClock() = default;
CDVDClock::~CDVDClock() = default;

double CDVDClock::GetAbsoluteClock(bool interpolated)
{
  (void)interpolated;
  return 0.0;
}

double CDVDClock::GetClock(bool interpolated)
{
  (void)interpolated;
  return 0.0;
}

double CDVDClock::GetVsyncAdjust()
{
  return 0.0;
}

double CDVDClock::GetClockSpeed() const
{
  return 1.0;
}

// Used for one log line.
extern "C" const char* avcodec_get_name(enum AVCodecID id)
{
  (void)id;
  return "test";
}

// Handing a packet to the sink never guesses a channel layout.
CAEChannelInfo CAEUtil::GuessChLayout(const unsigned int channels)
{
  ADD_FAILURE() << "preroll submission reached channel-layout guessing for " << channels;
  return {};
}

namespace
{
using Result = CAudioSinkAE::PrerollPacketResult;

class PrerollSink : public testing::Test
{
protected:
  void SetUp() override
  {
    g_engine = &engine;
    ON_CALL(*stream, GetFrameSize()).WillByDefault(Return(1));
    ON_CALL(*stream, GetSpace()).WillByDefault(Return(MAX_IEC61937_PACKET));
    ON_CALL(*stream, AddData(_, _, _, _))
        .WillByDefault(
            [this](const uint8_t* const*, unsigned int, unsigned int frames, IAEStream::ExtData*) {
              ++writes;
              if (!shortWrite)
                return frames;
              // Part of it once, then nothing more - which is what a stream
              // that fills up mid-packet looks like.
              return writes == 1 ? 7u : 0u;
            });
  }

  void TearDown() override { g_engine = nullptr; }

  // A whole, well-formed passthrough packet - the only thing preroll submits.
  DVDAudioFrame WholePacket() const
  {
    DVDAudioFrame frame{};
    frame.passthrough = true;
    frame.nb_frames = MAX_IEC61937_PACKET;
    frame.framesOut = 0;
    frame.format.m_dataFormat = AE_FMT_RAW;
    return frame;
  }

  // Built the way production builds it: the stream comes from the engine.
  std::unique_ptr<CAudioSinkAE> MakeSink()
  {
    EXPECT_CALL(engine, MakeStream(_, _, _))
        .WillOnce([this](AEAudioFormat&, unsigned int, IAEClockCallback*) {
          // The deleter is given the engine it must hand the stream back to,
          // which is the mock - so the fixture keeps ownership of it.
          return IAE::StreamPtr(stream.get(), IAEStreamDeleter(engine));
        });
    auto sink = std::make_unique<CAudioSinkAE>(&clock);
    EXPECT_TRUE(sink->Create(WholePacket(), AV_CODEC_ID_TRUEHD, false)) << "sink creation failed";
    return sink;
  }

  CDVDClock clock;
  NiceMock<MockEngine> engine;
  std::unique_ptr<NiceMock<MockStream>> stream{std::make_unique<NiceMock<MockStream>>()};
  unsigned int writes{0};
  bool shortWrite{false};
};

TEST_F(PrerollSink, WithNoRoomNothingIsWrittenAndTheCallerIsToldToWait)
{
  // Deferred has to mean nothing happened at all.
  EXPECT_CALL(*stream, GetSpace()).WillRepeatedly(Return(0));
  EXPECT_CALL(*stream, AddData(_, _, _, _)).Times(0);

  auto sink = MakeSink();
  auto frame = WholePacket();

  for (int attempt = 0; attempt < 100; ++attempt)
  {
    unsigned int written = 999;
    ASSERT_EQ(Result::Deferred, sink->AddPrerollPacket(frame, written)) << "attempt " << attempt;
    ASSERT_EQ(0u, written);
    ASSERT_EQ(0, frame.framesOut) << "a deferred packet advanced the frame";
  }
}

TEST_F(PrerollSink, WithRoomTheWholePacketGoesInOneWrite)
{
  auto sink = MakeSink();
  auto frame = WholePacket();
  unsigned int written = 0;

  EXPECT_EQ(Result::Written, sink->AddPrerollPacket(frame, written));
  EXPECT_EQ(static_cast<unsigned int>(MAX_IEC61937_PACKET), written);
  EXPECT_EQ(1u, writes) << "the packet was split across writes";
}

TEST_F(PrerollSink, AShortWriteIsAFailureRatherThanProgress)
{
  shortWrite = true;
  // The write loop waits the stream's reported delay plus the frame duration
  // plus two seconds before giving up on a stream that stops accepting.
  // Reporting a delay already elapsed puts that deadline in the past, so the
  // case exercises the give-up path without sitting through it.
  EXPECT_CALL(*stream, GetDelay()).WillRepeatedly(Return(-2.0));

  auto sink = MakeSink();
  auto frame = WholePacket();
  unsigned int written = 0;

  EXPECT_EQ(Result::Failed, sink->AddPrerollPacket(frame, written));
  EXPECT_EQ(7u, written) << "the short count was not reported";
}

class PrerollSinkBadSize : public PrerollSink, public testing::WithParamInterface<int>
{
};

TEST_P(PrerollSinkBadSize, OneSlotIsOneWholePacketAndNothingElse)
{
  auto sink = MakeSink();
  auto frame = WholePacket();
  frame.nb_frames = GetParam();
  unsigned int written = 9;

  EXPECT_EQ(Result::InvalidFormat, sink->AddPrerollPacket(frame, written));
  EXPECT_EQ(0u, written);
  EXPECT_EQ(0u, writes) << "an ill-sized packet reached the sink";
}

INSTANTIATE_TEST_SUITE_P(EmptyNegativeAndOversized,
                         PrerollSinkBadSize,
                         testing::Values(0, -1, MAX_IEC61937_PACKET + 1));

TEST_F(PrerollSink, APartlyConsumedFrameIsNotAPrerollPacket)
{
  auto sink = MakeSink();
  auto frame = WholePacket();
  frame.framesOut = 1;
  unsigned int written = 9;

  EXPECT_EQ(Result::InvalidFormat, sink->AddPrerollPacket(frame, written));
  EXPECT_EQ(0u, writes);
}

TEST_F(PrerollSink, PCMIsNotAPrerollPacket)
{
  auto sink = MakeSink();
  auto frame = WholePacket();
  frame.passthrough = false;
  unsigned int written = 9;

  EXPECT_EQ(Result::InvalidFormat, sink->AddPrerollPacket(frame, written));
  EXPECT_EQ(0u, writes);
}

TEST_F(PrerollSink, AStreamWhoseFrameIsNotOneByteIsNotARawCarrier)
{
  EXPECT_CALL(*stream, GetFrameSize()).WillRepeatedly(Return(2));

  auto sink = MakeSink();
  auto frame = WholePacket();
  unsigned int written = 9;

  EXPECT_EQ(Result::InvalidFormat, sink->AddPrerollPacket(frame, written));
  EXPECT_EQ(0u, writes);
}

TEST_F(PrerollSink, WithNoStreamAtAllTheSinkSaysSoRatherThanFailing)
{
  // Never created, so there is no stream behind it. Unavailable and Failed
  // mean different things to the caller: one is worth retrying, one is not.
  CAudioSinkAE sink(&clock);
  auto frame = WholePacket();
  unsigned int written = 9;

  EXPECT_EQ(Result::Unavailable, sink.AddPrerollPacket(frame, written));
}
} // namespace
