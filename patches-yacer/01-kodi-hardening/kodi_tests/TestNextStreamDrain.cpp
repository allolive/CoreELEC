/*
 *  Copyright (C) 2026 Team CoreELEC
 *  This file is part of CoreELEC - https://coreelec.org
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

// What the player decides each pass while a disc's old title plays out
// before the next one opens (01_08).

#include "cores/VideoPlayer/NextStreamDrain.h"

#include <gtest/gtest.h>

namespace
{
using Drain = CNextStreamDrain;
using State = CNextStreamDrain::State;
using namespace std::chrono_literals;

const Drain::Clock::time_point T0{100s};
constexpr bool PLAYING = false;
constexpr bool PAUSED = true;

// pts in DVD time units, the way the stream players report it
double Pts(std::chrono::milliseconds at)
{
  return static_cast<double>(at.count()) * 1000.0;
}

TEST(NextStreamDrain, NothingIsPendingUntilAStreamAsksForTheNext)
{
  Drain drain;
  EXPECT_FALSE(drain.Pending());
  EXPECT_EQ(State::DONE, drain.Poll(true, 0.0, PLAYING, T0));
}

TEST(NextStreamDrain, TheOldStreamsPlayOutForAsLongAsTheirPositionMoves)
{
  Drain drain;
  drain.Begin();
  // a title queued in full plays out well past the 40 s the queue's own
  // drain used to allow
  for (auto t = 0ms; t < 75s; t += 100ms)
    ASSERT_EQ(State::PLAYING_OUT, drain.Poll(true, Pts(t), PLAYING, T0 + t));
  EXPECT_TRUE(drain.Pending());
  EXPECT_EQ(State::DONE, drain.Poll(false, Pts(75s), PLAYING, T0 + 75s));
  EXPECT_FALSE(drain.Pending());
}

TEST(NextStreamDrain, ADrainWhosePositionStopsIsGivenUp)
{
  Drain drain;
  drain.Begin();
  EXPECT_EQ(State::PLAYING_OUT, drain.Poll(true, Pts(5s), PLAYING, T0));
  EXPECT_EQ(State::PLAYING_OUT, drain.Poll(true, Pts(5s), PLAYING, T0 + Drain::STALL));
  EXPECT_EQ(State::STALLED, drain.Poll(true, Pts(5s), PLAYING, T0 + Drain::STALL + 100ms));
  EXPECT_FALSE(drain.Pending());
}

TEST(NextStreamDrain, APauseDoesNotCountAgainstTheDrain)
{
  Drain drain;
  drain.Begin();
  EXPECT_EQ(State::PLAYING_OUT, drain.Poll(true, Pts(5s), PLAYING, T0));
  for (auto t = 1s; t <= 60s; t += 1s)
    ASSERT_EQ(State::PLAYING_OUT, drain.Poll(true, Pts(5s), PAUSED, T0 + t));
  // the stall window starts again when playback resumes
  EXPECT_EQ(State::PLAYING_OUT, drain.Poll(true, Pts(5s), PLAYING, T0 + 60s + Drain::STALL));
  EXPECT_EQ(State::STALLED, drain.Poll(true, Pts(5s), PLAYING, T0 + 61s + Drain::STALL));
}

TEST(NextStreamDrain, AnyOtherSpeedThatStopsThePositionCountsAsAStall)
{
  // rewind: the video player stops taking data and nothing seeks back while
  // the old title plays out, so the drain must not wait on it forever
  Drain drain;
  drain.Begin();
  EXPECT_EQ(State::PLAYING_OUT, drain.Poll(true, Pts(5s), PLAYING, T0));
  EXPECT_EQ(State::STALLED, drain.Poll(true, Pts(5s), PLAYING, T0 + Drain::STALL + 1s));
}

TEST(NextStreamDrain, AFirstPollIsNeverAStall)
{
  // a stream whose position has not moved yet, polled long after an earlier
  // drain left the same position behind
  Drain drain;
  drain.Begin();
  EXPECT_EQ(State::PLAYING_OUT, drain.Poll(true, Pts(5s), PLAYING, T0));
  EXPECT_EQ(State::DONE, drain.Poll(false, Pts(5s), PLAYING, T0 + 1s));
  drain.Begin();
  EXPECT_EQ(State::PLAYING_OUT, drain.Poll(true, Pts(5s), PLAYING, T0 + 100s));
  EXPECT_EQ(State::PLAYING_OUT, drain.Poll(true, Pts(5s), PLAYING, T0 + 100s + Drain::STALL));
}

TEST(NextStreamDrain, EmptyQueuesEndTheDrainAtOnce)
{
  Drain drain;
  drain.Begin();
  EXPECT_EQ(State::DONE, drain.Poll(false, 0.0, PLAYING, T0));
}

TEST(NextStreamDrain, ACancelledDrainIsNotPending)
{
  Drain drain;
  drain.Begin();
  EXPECT_EQ(State::PLAYING_OUT, drain.Poll(true, Pts(5s), PLAYING, T0));
  drain.Cancel();
  EXPECT_FALSE(drain.Pending());
}

} // namespace
