/*
 *  Copyright (C) 2026 Team CoreELEC
 *  This file is part of CoreELEC - https://coreelec.org
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

// The gate decides what happens to each decoded picture while a resume is
// being prepared: hold it, drop it, keep it as the one to show, or let it
// through as normal. Every transition is keyed on a generation, because a
// seek or a flush can replace the run at any point and a picture belonging to
// the previous attempt must not satisfy its replacement.

#include "cores/VideoPlayer/VideoRenderers/VideoPrerollGate.h"

#include <gtest/gtest.h>

namespace
{
using Gate = CVideoPrerollGate;
using Picture = Gate::Picture;

constexpr double TARGET = 1000000;

// A gate armed on TARGET, which is where most of the interesting states start.
Gate Armed(uint64_t generation = 2)
{
  Gate gate;
  EXPECT_TRUE(gate.Begin(generation, TARGET));
  EXPECT_EQ(generation, gate.ArmAfterFlush());
  return gate;
}

TEST(VideoPrerollGate, WithoutATargetEverythingIsHeldAndNothingReleases)
{
  Gate gate;
  ASSERT_TRUE(gate.Begin(1, DVD_NOPTS_VALUE));
  EXPECT_EQ(Picture::HOLD, gate.Check(0, 123, true));
  EXPECT_EQ(1u, gate.ArmAfterFlush());
  EXPECT_EQ(Picture::HOLD, gate.Check(1, 123, true));
  EXPECT_FALSE(gate.Read().ready) << "a gate with no target reported a picture to show";
  EXPECT_FALSE(gate.Release(1));
}

TEST(VideoPrerollGate, PicturesFromAnotherGenerationAreDroppedNotHeld)
{
  Gate gate;
  ASSERT_TRUE(gate.Begin(2, TARGET));
  // Still the previous generation's pictures: they belong to a run that no
  // longer exists, so holding them would keep the wrong frame.
  EXPECT_EQ(Picture::DROP, gate.Check(1, TARGET, true));
  EXPECT_EQ(Picture::HOLD, gate.Check(0, TARGET, true));
}

TEST(VideoPrerollGate, OnlyAPictureAtTheTargetIsACandidate)
{
  auto gate = Armed();
  EXPECT_EQ(Picture::DROP, gate.Check(2, TARGET - 1, true)) << "a picture before the target was kept";
  EXPECT_EQ(Picture::CANDIDATE, gate.Check(2, TARGET, true));
}

TEST(VideoPrerollGate, RetainingACandidatePublishesItAndStopsFurtherCandidates)
{
  auto gate = Armed();
  ASSERT_EQ(Picture::CANDIDATE, gate.Check(2, TARGET, true));
  ASSERT_TRUE(gate.Retain(2, TARGET));

  EXPECT_TRUE(gate.Read().ready);
  EXPECT_EQ(TARGET, gate.Read().firstPts);
  EXPECT_EQ(Picture::HOLD, gate.Check(2, TARGET, true)) << "a second candidate was taken";
}

TEST(VideoPrerollGate, AForeignGenerationCanNeitherReleaseNorCancel)
{
  auto gate = Armed();
  ASSERT_EQ(Picture::CANDIDATE, gate.Check(2, TARGET, true));
  ASSERT_TRUE(gate.Retain(2, TARGET));

  EXPECT_FALSE(gate.Release(1));
  EXPECT_FALSE(gate.Cancel(1));
  EXPECT_TRUE(gate.Release(2)) << "the owning generation could not release";
}

TEST(VideoPrerollGate, ReleaseSchedulingAndSubmissionAreSeparateSteps)
{
  auto gate = Armed();
  ASSERT_EQ(Picture::CANDIDATE, gate.Check(2, TARGET, true));
  ASSERT_TRUE(gate.Retain(2, TARGET));
  ASSERT_TRUE(gate.Release(2));

  EXPECT_TRUE(gate.AwaitingPresentation());
  EXPECT_EQ(Picture::NORMAL, gate.Check(2, TARGET, true)) << "the released picture was still held";

  gate.Scheduled();
  EXPECT_FALSE(gate.AwaitingPresentation());
  EXPECT_TRUE(gate.AwaitingSubmission());
  EXPECT_FALSE(gate.Read().submitted);

  gate.Submitted(1);
  EXPECT_FALSE(gate.Read().submitted) << "another generation's submission was accepted";

  gate.Submitted(2);
  EXPECT_TRUE(gate.Read().submitted);
  EXPECT_FALSE(gate.AwaitingSubmission());
}

TEST(VideoPrerollGate, OnceHandedOverALaterFailureCannotReviveTheHold)
{
  auto gate = Armed();
  ASSERT_EQ(Picture::CANDIDATE, gate.Check(2, TARGET, true));
  ASSERT_TRUE(gate.Retain(2, TARGET));
  ASSERT_TRUE(gate.Release(2));
  gate.Scheduled();
  gate.Submitted(2);

  // Ordinary delay and display events keep arriving afterwards.
  gate.Fail();
  EXPECT_FALSE(gate.Read().failed);
  EXPECT_TRUE(gate.CanOutput(2));
  EXPECT_EQ(Picture::NORMAL, gate.Check(2, TARGET + 41667, true));
}

TEST(VideoPrerollGate, CancellingReturnsToOrdinaryOutput)
{
  auto gate = Armed();
  ASSERT_EQ(Picture::CANDIDATE, gate.Check(2, TARGET, true));
  ASSERT_TRUE(gate.Retain(2, TARGET));
  ASSERT_TRUE(gate.Release(2));
  gate.Scheduled();
  gate.Submitted(2);

  ASSERT_TRUE(gate.Cancel(2));
  EXPECT_EQ(Picture::DROP, gate.Check(2, TARGET, true)) << "a cancelled run still accepted pictures";
  EXPECT_EQ(0u, gate.ArmAfterFlush()) << "a cancelled run re-armed itself";
  EXPECT_EQ(Picture::NORMAL, gate.Check(0, TARGET, true));
}

TEST(VideoPrerollGate, APictureThatCannotBeHeldFailsTheRun)
{
  auto gate = Armed(3);
  EXPECT_EQ(Picture::HOLD, gate.Check(3, TARGET, false));
  EXPECT_TRUE(gate.Read().failed) << "an unholdable picture did not fail the run";
  EXPECT_FALSE(gate.Retain(3, TARGET)) << "a failed run still retained a picture";
  EXPECT_FALSE(gate.Release(3)) << "a failed run still released";
}

TEST(VideoPrerollGate, CancellingClearsAFailureSoTheNextRunStartsClean)
{
  auto gate = Armed(4);
  gate.Fail();
  ASSERT_TRUE(gate.Read().failed);
  EXPECT_FALSE(gate.Release(4));

  ASSERT_TRUE(gate.Cancel(4));
  EXPECT_FALSE(gate.Read().failed);
  EXPECT_EQ(Picture::NORMAL, gate.Check(0, TARGET, true));
}

TEST(VideoPrerollGate, AFailureBetweenSchedulingAndSubmissionIsNotOverwritten)
{
  Gate gate;
  ASSERT_TRUE(gate.Begin(1, TARGET));
  ASSERT_EQ(1u, gate.ArmAfterFlush());
  ASSERT_TRUE(gate.Retain(1, TARGET));
  ASSERT_TRUE(gate.Release(1));
  gate.Scheduled();

  // The handoff failed after it was scheduled but before it landed.
  gate.Fail();
  gate.Submitted(1);
  EXPECT_TRUE(gate.Read().failed) << "a late submission cleared the failure";
  EXPECT_FALSE(gate.Read().submitted) << "a failed handoff was recorded as submitted";
}
} // namespace
