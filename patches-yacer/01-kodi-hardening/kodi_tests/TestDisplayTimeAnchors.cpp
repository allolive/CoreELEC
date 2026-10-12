/*
 *  Copyright (C) 2026 Team CoreELEC
 *  This file is part of CoreELEC - https://coreelec.org
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

// Which of a disc's times belongs to the picture on screen, when the reader
// is a title ahead of it (01_09).

#include "cores/VideoPlayer/DisplayTimeAnchors.h"

#include <gtest/gtest.h>

namespace
{
constexpr double Sec(double s)
{
  return s * DVD_TIME_BASE;
}

// what the player shows: the clock plus the offset, in ms
struct Shown
{
  double time;
  int total;
};

Shown Show(CDisplayTimeAnchors& anchors, double clock)
{
  double offset = 0.0;
  int total = 0;
  EXPECT_TRUE(anchors.Get(clock, offset, total));
  return {(clock + offset) * 1000 / DVD_TIME_BASE, total};
}

// a title read packet by packet: dts from start, display time from 40 ms on
void Read(CDisplayTimeAnchors& anchors, double start, double length, int total)
{
  for (double t = 0.04; t < length; t += 0.04)
    anchors.Add(Sec(start + t), static_cast<int>(t * 1000), total);
}

TEST(DisplayTimeAnchors, NothingKnownLeavesTheLastPacketRead)
{
  CDisplayTimeAnchors anchors;
  double offset = 7.0;
  int total = 9;
  EXPECT_FALSE(anchors.Get(Sec(10), offset, total));
  EXPECT_EQ(7.0, offset);
  EXPECT_EQ(9, total);
}

TEST(DisplayTimeAnchors, ATitleShowsTheTimeOfThePictureOnScreen)
{
  CDisplayTimeAnchors anchors;
  Read(anchors, 10, 123, 123000);
  const Shown shown = Show(anchors, Sec(70));
  EXPECT_NEAR(60000, shown.time, 1);
  EXPECT_EQ(123000, shown.total);
}

TEST(DisplayTimeAnchors, ThePictureKeepsItsTitleWhileTheReaderIsOneAhead)
{
  // a 2:03 title, then a 1:03 one read two
  // minutes ahead of the picture. Taken from the last packet read, the
  // picture at 2:00 of the first title showed as -0:03 of 1:03.
  CDisplayTimeAnchors anchors;
  Read(anchors, 10, 123, 123000);
  Read(anchors, 133, 7, 63000);

  Shown shown = Show(anchors, Sec(130));
  EXPECT_NEAR(120000, shown.time, 1);
  EXPECT_EQ(123000, shown.total);

  // the new title takes over when its first picture is due
  shown = Show(anchors, Sec(138));
  EXPECT_NEAR(5000, shown.time, 1);
  EXPECT_EQ(63000, shown.total);
}

TEST(DisplayTimeAnchors, AStepOfLessThanASecondIsTheSameTitle)
{
  // the demuxer re-bases the display time on whichever stream's packet
  // follows a NAV pack, so within a title it wobbles by the audio-video gap
  CDisplayTimeAnchors anchors;
  anchors.Add(Sec(10), 1000, 60000);
  anchors.Add(Sec(11), 2300, 60000);
  anchors.Add(Sec(12), 2700, 60000);
  anchors.Add(Sec(13), 4400, 60000);
  double offset = 0.0;
  int total = 0;
  ASSERT_TRUE(anchors.Get(Sec(13), offset, total));
  EXPECT_EQ(Sec(1) - Sec(10), offset);
}

TEST(DisplayTimeAnchors, ATimelineThatStartsAgainDropsTheOldAnchors)
{
  CDisplayTimeAnchors anchors;
  anchors.Add(Sec(100), 50000, 60000);
  anchors.Add(Sec(5), 3000, 90000);
  const Shown shown = Show(anchors, Sec(6));
  EXPECT_NEAR(4000, shown.time, 1);
  EXPECT_EQ(90000, shown.total);
}

TEST(DisplayTimeAnchors, AStepBackKeepsTheAnchorsBeforeIt)
{
  // a wrap back too small for the player to correct, at a seamless join
  CDisplayTimeAnchors anchors;
  Read(anchors, 10, 123, 123000);
  anchors.Add(Sec(140), 7000, 63000);
  anchors.Add(Sec(139.5), 3000, 63000);
  const Shown shown = Show(anchors, Sec(130));
  EXPECT_NEAR(120000, shown.time, 1);
  EXPECT_EQ(123000, shown.total);
}

TEST(DisplayTimeAnchors, PacketsWithoutATimeAreNoAnchor)
{
  CDisplayTimeAnchors anchors;
  anchors.Add(DVD_NOPTS_VALUE, 5000, 60000);
  anchors.Add(Sec(5), 0, 60000);
  double offset = 0.0;
  int total = 0;
  EXPECT_FALSE(anchors.Get(Sec(5), offset, total));
}

TEST(DisplayTimeAnchors, AFlushForgetsThem)
{
  CDisplayTimeAnchors anchors;
  Read(anchors, 10, 5, 60000);
  anchors.Clear();
  double offset = 0.0;
  int total = 0;
  EXPECT_FALSE(anchors.Get(Sec(12), offset, total));
}

TEST(DisplayTimeAnchors, TheAnchorInUseIsNeverTheOneGivenUp)
{
  // a disc that loops short cells far ahead of a picture standing still
  CDisplayTimeAnchors anchors;
  for (int i = 0; i < 1000; ++i)
    anchors.Add(Sec(10 + i), 1000 + (i % 2) * 5000, 60000);
  double offset = 0.0;
  int total = 0;
  ASSERT_TRUE(anchors.Get(Sec(10), offset, total));
  EXPECT_EQ(Sec(1) - Sec(10), offset);
  // the newest are kept too: anchor 900 of 1000
  ASSERT_TRUE(anchors.Get(Sec(910), offset, total));
  EXPECT_EQ(Sec(1) - Sec(910), offset);
}

} // namespace
