/*
 *  Copyright (C) 2026 Team CoreELEC
 *  This file is part of CoreELEC - https://coreelec.org
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

// Preroll decides how much real audio has been handed to the sink before a
// resume is allowed. Getting that wrong in the optimistic direction is what
// releases a resume too early, so most of what is pinned here is the refusal:
// invented timestamps, partial writes, gaps, and stale sync evidence.

#include "cores/VideoPlayer/AudioPreroll.h"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace
{
using Failure = AudioPrerollStatus::Failure;
using Phase = AudioPrerollStatus::Phase;

// One whole packet accepted end to end, which is what the runway is made of.
void Feed(CAudioPreroll& preroll, double pts, double duration)
{
  ASSERT_FALSE(preroll.HoldPacket(pts, duration, true, true)) << "a valid packet was held";
  ASSERT_TRUE(preroll.Accepted(pts, duration, 61440, 61440, 0)) << "a complete packet was refused";
}

TEST(AudioPreroll, WithNoPacketsThereIsNoFirstTimestampToReport)
{
  CAudioPreroll preroll;
  ASSERT_TRUE(preroll.Begin(1, 500000));
  const auto status = preroll.Observe({}, 100);
  EXPECT_EQ(DVD_NOPTS_VALUE, status.firstPts) << "a first PTS was invented";
  EXPECT_FALSE(preroll.Buffered());
}

TEST(AudioPreroll, AZeroTimestampIsAValidPositionRatherThanAMissingOne)
{
  CAudioPreroll preroll;
  ASSERT_TRUE(preroll.Begin(1, 500000));
  Feed(preroll, 0, 20000);

  const auto status = preroll.Observe({30000, true, false, true}, 200, Phase::Buffered);
  EXPECT_EQ(Phase::Buffered, status.phase);
  EXPECT_EQ(0, status.firstPts) << "a zero first PTS was treated as absent";
  EXPECT_EQ(20000, status.submittedEndPts);
  EXPECT_EQ(-10000, status.estimatedPlayingPts);
  EXPECT_EQ(200, status.hostUs);
  EXPECT_FALSE(status.inSync) << "a paused stream was reported in sync";
}

TEST(AudioPreroll, APacketEndingExactlyAtTheTargetIsSubmittedButTheNextIsHeld)
{
  CAudioPreroll preroll;
  preroll.Begin(1, 500000);
  Feed(preroll, 460000, 20000);
  Feed(preroll, 480000, 20000); // Half-open: ends exactly at the target.

  ASSERT_TRUE(preroll.HoldPacket(500000, 20000, true, true)) << "the main packet was not held";
  const auto status = preroll.Observe({120000, true, true, false}, 100);
  EXPECT_EQ(Phase::AtBoundary, status.phase);
  EXPECT_TRUE(status.mainHeld);
  EXPECT_EQ(500000, status.submittedEndPts) << "a held packet advanced the accepted end";
}

TEST(AudioPreroll, OnlyAForwardTargetOnTheCurrentGenerationReopensAHeldPacket)
{
  CAudioPreroll preroll;
  preroll.Begin(1, 500000);
  Feed(preroll, 460000, 20000);
  Feed(preroll, 480000, 20000);
  ASSERT_TRUE(preroll.HoldPacket(500000, 20000, true, true));

  EXPECT_FALSE(preroll.SetTarget(2, 520000)) << "a stale generation moved the target";
  EXPECT_TRUE(preroll.Held());
  EXPECT_FALSE(preroll.SetTarget(1, 499999)) << "the target moved backwards";
  EXPECT_TRUE(preroll.Held());

  EXPECT_TRUE(preroll.SetTarget(1, 520000));
  EXPECT_FALSE(preroll.Held()) << "a forward target did not reopen the packet";
  Feed(preroll, 500000, 20000);
}

TEST(AudioPreroll, AFractionalPacketStraddlingTheTargetIsHeldNotSubmitted)
{
  CAudioPreroll preroll;
  preroll.Begin(1, 500000);
  Feed(preroll, 460000, 21768.707482993197);

  EXPECT_TRUE(preroll.HoldPacket(481768.707482993197, 21768.707482993197, true, true));
  EXPECT_TRUE(preroll.Held());
  EXPECT_FALSE(preroll.Failed());
}

TEST(AudioPreroll, SyncEvidenceMustBeFreshResumedAndSettled)
{
  CAudioPreroll preroll;
  preroll.Begin(1, 500000);
  Feed(preroll, 0, 20000);

  EXPECT_FALSE(preroll.Observe({10000, true, false, true}, 100).inSync) << "paused accepted";
  EXPECT_FALSE(preroll.Observe({10000, true, true, true}, 200).inSync) << "stale accepted";
  EXPECT_FALSE(preroll.Observe({10000, true, true, false}, 300).inSync) << "acquiring accepted";

  const auto ready = preroll.Observe({5000, true, true, true}, 400);
  EXPECT_TRUE(ready.inSync);
  EXPECT_EQ(15000, ready.estimatedPlayingPts) << "position did not advance on fresh evidence";
  EXPECT_EQ(400, ready.hostUs);

  EXPECT_FALSE(preroll.Observe({0, true, false, true}, 500).inSync) << "a repause stayed ready";
  EXPECT_FALSE(preroll.Observe({0, true, true, true}, 600).inSync) << "a re-resume reused a witness";
}

TEST(AudioPreroll, AnUnusableDelayDoesNotFabricateProgress)
{
  CAudioPreroll preroll;
  preroll.Begin(1, 500000);
  Feed(preroll, 0, 20000);
  const auto status =
      preroll.Observe({std::numeric_limits<double>::quiet_NaN(), true, true, true}, 700);
  EXPECT_EQ(DVD_NOPTS_VALUE, status.estimatedPlayingPts);
}

class AudioPrerollPartialWrite : public testing::TestWithParam<unsigned int>
{
};

TEST_P(AudioPrerollPartialWrite, AWriteThatIsNotExactlyTheWholePacketFailsTheRun)
{
  CAudioPreroll preroll;
  preroll.Begin(1, 500000);
  Feed(preroll, 0, 20000);

  ASSERT_FALSE(preroll.Accepted(20000, 20000, GetParam(), 61440, 0)) << "partial write accepted";
  const auto status = preroll.Observe({0, true, true, true}, 100);
  EXPECT_EQ(Phase::Failed, status.phase);
  EXPECT_EQ(20000, status.submittedEndPts) << "a failed write gained media progress";
  EXPECT_FALSE(status.inSync);
  EXPECT_FALSE(preroll.End(1)) << "a failed run was still allowed to release";
}

INSTANTIATE_TEST_SUITE_P(ShortAndOverlongWrites,
                         AudioPrerollPartialWrite,
                         testing::Values(0U, 1U, 61439U, 61441U));

TEST(AudioPreroll, APartialByteContinuationDoesNotCreditAFrame)
{
  CAudioPreroll preroll;
  preroll.Begin(1, 500000);
  EXPECT_FALSE(preroll.Accepted(0, 20000, 61439, 61440, 1));
}

class AudioPrerollBadPts : public testing::TestWithParam<double>
{
};

TEST_P(AudioPrerollBadPts, APacketWithoutAUsableTimestampIsRefused)
{
  CAudioPreroll preroll;
  preroll.Begin(1, 500000);
  EXPECT_TRUE(preroll.HoldPacket(GetParam(), 20000, true, true));
  EXPECT_TRUE(preroll.Failed()) << "an unusable timestamp was consumed";
}

INSTANTIATE_TEST_SUITE_P(EveryUnusableTimestamp,
                         AudioPrerollBadPts,
                         testing::Values(static_cast<double>(DVD_NOPTS_VALUE),
                                         std::numeric_limits<double>::quiet_NaN(),
                                         std::numeric_limits<double>::infinity()));

class AudioPrerollBadDuration : public testing::TestWithParam<double>
{
};

TEST_P(AudioPrerollBadDuration, APacketWithoutAUsableDurationIsRefused)
{
  CAudioPreroll preroll;
  preroll.Begin(1, 500000);
  EXPECT_TRUE(preroll.HoldPacket(0, GetParam(), true, true));
  EXPECT_TRUE(preroll.Failed()) << "an unusable duration was consumed";
}

INSTANTIATE_TEST_SUITE_P(EveryUnusableDuration,
                         AudioPrerollBadDuration,
                         testing::Values(0.0, -1.0, std::numeric_limits<double>::quiet_NaN()));

TEST(AudioPreroll, AnExtrapolatedTimestampIsNotEvidence)
{
  CAudioPreroll preroll;
  preroll.Begin(1, 500000);
  EXPECT_TRUE(preroll.HoldPacket(0, 20000, false, true));
  EXPECT_TRUE(preroll.Failed()) << "an extrapolated PTS was authorised";
}

TEST(AudioPreroll, PCMIsNotAPassthroughPreroll)
{
  CAudioPreroll preroll;
  preroll.Begin(1, 500000);
  EXPECT_TRUE(preroll.HoldPacket(0, 20000, true, false));
  EXPECT_TRUE(preroll.Failed());
}

TEST(AudioPreroll, StartingTooCloseToTheTargetLeavesNoRunway)
{
  CAudioPreroll preroll;
  preroll.Begin(1, 500000);
  EXPECT_TRUE(preroll.HoldPacket(490000, 20000, true, true));
  EXPECT_TRUE(preroll.Failed()) << "a missing repeated lead was accepted";
}

TEST(AudioPreroll, AGenerationMustBeRealAndItsTargetKnown)
{
  CAudioPreroll preroll;
  EXPECT_FALSE(preroll.Begin(0, 500000)) << "generation zero accepted";
  EXPECT_FALSE(preroll.Begin(1, DVD_NOPTS_VALUE)) << "an unknown target accepted";
}

TEST(AudioPreroll, OnlyTheOwningGenerationCanEndOrCancelTheRun)
{
  CAudioPreroll preroll;
  preroll.Begin(1, 500000);
  Feed(preroll, 0, 20000);

  EXPECT_FALSE(preroll.End(2)) << "a foreign generation ended the transaction";
  EXPECT_FALSE(preroll.Cancel(2)) << "a foreign generation cancelled the transaction";
  EXPECT_TRUE(preroll.Cancel(1));
  EXPECT_FALSE(preroll.Active());
  EXPECT_FALSE(preroll.Begin(1, 500000)) << "a cancelled generation restarted";
}

TEST(AudioPreroll, AReplacementGenerationInheritsNeitherPositionNorReadiness)
{
  CAudioPreroll preroll;
  preroll.Begin(1, 500000);
  Feed(preroll, 0, 20000);
  ASSERT_TRUE(preroll.Cancel(1));

  ASSERT_TRUE(preroll.Begin(2, 900000)) << "the replacement generation failed";
  auto status = preroll.Observe({0, true, true, true}, 100);
  EXPECT_EQ(2u, status.generation);
  EXPECT_EQ(DVD_NOPTS_VALUE, status.firstPts) << "the replacement inherited a position";
  EXPECT_FALSE(status.inSync) << "the replacement inherited sync readiness";

  Feed(preroll, 400000, 20000);
  EXPECT_TRUE(preroll.End(2));
  EXPECT_FALSE(preroll.Active());
  EXPECT_FALSE(preroll.Observe({0, true, true, true}, 200).inSync) << "an ended run stayed ready";
}

class AudioPrerollDiscontinuity : public testing::TestWithParam<double>
{
};

TEST_P(AudioPrerollDiscontinuity, AGapOrOverlapDoesNotCountAsContiguousRunway)
{
  CAudioPreroll preroll;
  preroll.Begin(1, 1000000);
  Feed(preroll, 500000, 20000);
  EXPECT_TRUE(preroll.HoldPacket(GetParam(), 20000, true, true));
  EXPECT_TRUE(preroll.Failed());
}

// Just past the allowance either side, and a whole frame away.
INSTANTIATE_TEST_SUITE_P(JustOutsideTheAllowance,
                         AudioPrerollDiscontinuity,
                         testing::Values(522001.0, 517999.0, 900000.0));

TEST(AudioPreroll, ContainerRoundingDoesNotChangeTheCreditedDuration)
{
  CAudioPreroll preroll;
  preroll.Begin(1, 1000000);
  const double frame = 512.0 / 48000.0 * DVD_TIME_BASE;
  for (unsigned int i = 0; i < 40; ++i)
    Feed(preroll, std::round(i * frame / 1000.0) * 1000.0, frame);

  const auto status = preroll.Observe({0, true, true, false}, 100);
  EXPECT_NEAR(40 * frame, status.submittedEndPts, 0.001);
}

TEST(AudioPreroll, AGapThatRepeatsEveryFrameIsCaughtWithinAFewFrames)
{
  // Drift, not rounding: however small each step is, it must not accumulate
  // into runway that does not exist.
  CAudioPreroll preroll;
  preroll.Begin(1, 1000000);
  bool caught = false;
  for (unsigned int i = 0; i < 8 && !caught; ++i)
  {
    const double pts = i * 21000.0;
    if (preroll.HoldPacket(pts, 20000, true, true))
      caught = preroll.Failed();
    else
      preroll.Accepted(pts, 20000, 1, 1, 0);
  }
  EXPECT_TRUE(caught) << "small per-frame gaps accumulated fictitious media";
}

// Packet timestamps recorded from DTS trials on the box. An earlier helper
// rejected the final packet of each by less than a tenth of a microsecond.
class AudioPrerollRecordedDts : public testing::TestWithParam<std::vector<double>>
{
};

TEST_P(AudioPrerollRecordedDts, RealContainerQuantizationIsCreditedInFull)
{
  const double duration = 512.0 / 48000.0 * DVD_TIME_BASE;
  CAudioPreroll preroll;
  preroll.Begin(1, 129000000);
  for (double pts : GetParam())
    Feed(preroll, pts, duration);

  const auto status = preroll.Observe({}, 100);
  EXPECT_NEAR(GetParam().front() + GetParam().size() * duration, status.submittedEndPts, 0.001);
}

INSTANTIATE_TEST_SUITE_P(
    FromTheBox,
    AudioPrerollRecordedDts,
    testing::Values(std::vector<double>{127605000, 127616000, 127626000, 127637000, 127648000,
                                        127658000, 127669000, 127680000, 127690000, 127701000,
                                        127711000, 127722000, 127732000},
                    std::vector<double>{127680000, 127690000, 127701000, 127711000}));

class AudioPrerollSlackSign : public testing::TestWithParam<double>
{
};

TEST_P(AudioPrerollSlackSign, RoundoffIsAdmittedButAMeasurableExcessIsNot)
{
  const double sign = GetParam();
  const double duration = 512.0 / 48000.0 * DVD_TIME_BASE;
  CAudioPreroll preroll;
  preroll.Begin(1, 129000000);
  Feed(preroll, 127680000, duration);

  auto status = preroll.Observe({}, 100);
  EXPECT_TRUE(preroll.Accepted(status.submittedEndPts + sign * 2000.0000001, duration, 100, 100, 0))
      << "a numerical quantization edge was rejected";

  status = preroll.Observe({}, 200);
  EXPECT_TRUE(preroll.HoldPacket(status.submittedEndPts + sign * 2000.01, duration, true, true));
  EXPECT_TRUE(preroll.Failed()) << "numerical slack widened the continuity policy";
}

INSTANTIATE_TEST_SUITE_P(EitherSide, AudioPrerollSlackSign, testing::Values(-1.0, 1.0));

TEST(AudioPreroll, TheFirstFailureKeepsItsEvidenceAndIsNotOverwritten)
{
  CAudioPreroll preroll;
  preroll.Begin(1, 500000);
  Feed(preroll, 0, 20000);
  ASSERT_TRUE(preroll.HoldPacket(43000, 20000, true, true, 61440, 0));

  auto status = preroll.Observe({}, 100);
  EXPECT_EQ(Failure::Continuity, status.failure);
  EXPECT_EQ(43000, status.packetPts);
  EXPECT_EQ(20000, status.packetDuration);
  EXPECT_EQ(20000, status.expectedPts);
  EXPECT_EQ(61440u, status.packetBytes);
  EXPECT_EQ(0u, status.writtenBytes);

  preroll.Fail(Failure::Aborted);
  status = preroll.Observe({}, 200);
  EXPECT_EQ(Failure::Continuity, status.failure) << "a later failure replaced the original";
  EXPECT_EQ(43000, status.packetPts);
}

TEST(AudioPreroll, ANewGenerationDoesNotInheritFailureEvidence)
{
  CAudioPreroll preroll;
  preroll.Begin(1, 500000);
  Feed(preroll, 0, 20000);
  ASSERT_TRUE(preroll.HoldPacket(43000, 20000, true, true, 61440, 0));
  preroll.Cancel(1);

  preroll.Begin(2, 500000);
  const auto status = preroll.Observe({}, 300);
  EXPECT_EQ(Failure::None, status.failure);
  EXPECT_EQ(DVD_NOPTS_VALUE, status.packetPts);
}

TEST(AudioPreroll, APartialWriteKeepsItsByteAccounting)
{
  CAudioPreroll preroll;
  preroll.Begin(1, 500000);
  ASSERT_FALSE(preroll.Accepted(0, 20000, 7, 61440, 0)) << "a partial packet was credited";

  const auto status = preroll.Observe({}, 400);
  EXPECT_EQ(Failure::PartialWrite, status.failure);
  EXPECT_EQ(61440u, status.packetBytes);
  EXPECT_EQ(7u, status.writtenBytes);
  EXPECT_EQ(0u, status.previousBytes);
}

TEST(AudioPreroll, EveryFailureHasAReadableName)
{
  for (const auto failure : {Failure::Timestamp, Failure::Duration, Failure::Format,
                             Failure::NoLead, Failure::PastTarget, Failure::NotInSync,
                             Failure::Aborted, Failure::SinkUnavailable})
    EXPECT_STRNE("unknown", CAudioPreroll::FailureName(failure))
        << "a failure reaches the log with no diagnostic";
}
} // namespace
