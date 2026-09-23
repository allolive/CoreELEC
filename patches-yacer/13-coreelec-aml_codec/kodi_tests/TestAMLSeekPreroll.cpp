/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  The drop bookkeeping that discards the pictures a seek asked for. The
 *  decoder is asked to drop, keeps feeding packets, and every picture those
 *  packets produce has to be discarded - but the driver synthesises its own
 *  timestamps for untimestamped input, so the count is the only handle on
 *  those. Hardware measurement put the seek deficit at 432ms before this and
 *  57ms after, so the arithmetic below is load-bearing.
 */

#include "cores/VideoPlayer/DVDCodecs/Video/AMLSeekPreroll.h"

#include <limits>
#include <optional>

#include <gtest/gtest.h>

namespace
{
constexpr std::optional<double> NONE = std::nullopt;
}

TEST(AMLSeekPreroll, KeepsEverythingWhenNoDropWasAsked)
{
  CAMLSeekPreroll preroll;
  EXPECT_FALSE(preroll.ShouldDrop(1000.0));
  EXPECT_FALSE(preroll.ShouldDrop(NONE));
}

TEST(AMLSeekPreroll, IgnoresPacketsWhileNoDropIsAsked)
{
  CAMLSeekPreroll preroll;
  preroll.AcceptPacket(5000.0);
  // The packet was never part of a drop, so it must not arm a cutoff.
  EXPECT_FALSE(preroll.ShouldDrop(1000.0));
}

TEST(AMLSeekPreroll, DropsEverythingWhileTheDropIsAsked)
{
  CAMLSeekPreroll preroll;
  preroll.SetDrop(true);
  EXPECT_TRUE(preroll.ShouldDrop(1000.0));
  EXPECT_TRUE(preroll.ShouldDrop(NONE));
}

TEST(AMLSeekPreroll, DropsUpToTheLastPacketAcceptedAndThenStops)
{
  CAMLSeekPreroll preroll;
  preroll.SetDrop(true);
  preroll.AcceptPacket(1000.0);
  preroll.AcceptPacket(2000.0);
  preroll.SetDrop(false);

  // Pictures from the packets fed during the drop still have to go.
  EXPECT_TRUE(preroll.ShouldDrop(1000.0));
  EXPECT_TRUE(preroll.ShouldDrop(2000.0));
  // The first picture past the cutoff is the one the viewer asked for.
  EXPECT_FALSE(preroll.ShouldDrop(2001.0));
}

TEST(AMLSeekPreroll, TakesTheHighestTimestampAsTheCutoff)
{
  CAMLSeekPreroll preroll;
  preroll.SetDrop(true);
  // Packets arrive in decode order, which is not display order.
  preroll.AcceptPacket(3000.0);
  preroll.AcceptPacket(1000.0);
  preroll.SetDrop(false);
  EXPECT_TRUE(preroll.ShouldDrop(2500.0));
  EXPECT_FALSE(preroll.ShouldDrop(3001.0));
}

TEST(AMLSeekPreroll, StopsDroppingOnceAPictureHasPassed)
{
  CAMLSeekPreroll preroll;
  preroll.SetDrop(true);
  preroll.AcceptPacket(1000.0);
  preroll.SetDrop(false);
  ASSERT_FALSE(preroll.ShouldDrop(2000.0));
  // The cutoff is spent: a later picture below it must not be dropped again.
  EXPECT_FALSE(preroll.ShouldDrop(500.0));
}

TEST(AMLSeekPreroll, CountsPicturesWhenThePacketsCarriedNoTimestamp)
{
  CAMLSeekPreroll preroll;
  preroll.SetDrop(true);
  preroll.AcceptPacket(NONE);
  preroll.AcceptPacket(NONE);
  preroll.SetDrop(false);

  // The driver invents timestamps for these, so only the count identifies
  // them: exactly as many pictures go as packets were fed.
  EXPECT_TRUE(preroll.ShouldDrop(10.0));
  EXPECT_TRUE(preroll.ShouldDrop(20.0));
  EXPECT_FALSE(preroll.ShouldDrop(30.0));
}

// Pins a known fragility rather than endorsing it: with a cutoff armed, a
// picture carrying no timestamp is dropped whatever its position, because
// there is nothing to compare. If that is ever changed it should be a
// decision, not a surprise.
TEST(AMLSeekPreroll, DropsAnUntimestampedPictureWhileACutoffStands)
{
  CAMLSeekPreroll preroll;
  preroll.SetDrop(true);
  preroll.AcceptPacket(1000.0);
  preroll.SetDrop(false);
  EXPECT_TRUE(preroll.ShouldDrop(NONE));
}

TEST(AMLSeekPreroll, ForgetsEverythingOnReset)
{
  CAMLSeekPreroll preroll;
  preroll.SetDrop(true);
  preroll.AcceptPacket(1000.0);
  preroll.AcceptPacket(NONE);
  preroll.Reset();
  // A flush abandons the drop entirely; nothing may be left owing.
  EXPECT_FALSE(preroll.ShouldDrop(10.0));
  EXPECT_FALSE(preroll.ShouldDrop(NONE));
}

TEST(AMLSeekPreroll, DoesNotWrapTheOutstandingPictureCount)
{
  CAMLSeekPreroll preroll;
  preroll.SetDrop(true);
  // Far more packets than any real seek, to reach the saturation guard.
  for (unsigned int i = 0; i < 100000; ++i)
    preroll.AcceptPacket(NONE);
  preroll.SetDrop(false);
  // Still counting down, not wrapped to zero and released early.
  EXPECT_TRUE(preroll.ShouldDrop(NONE));
}
