/*
 *  Copyright (C) 2026 Team CoreELEC
 *  This file is part of CoreELEC - https://coreelec.org
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

// The error window is driven by a clock, so the clock is a parameter here
// rather than something to wait for: CSyncError takes its timer as a template
// argument, which is the cheapest seam there is. The last case replays a
// resume through all three pieces at once - window, observations and settling.

#include "cores/AudioEngine/Utils/AESyncError.h"
#include "cores/AudioEngine/Utils/AESyncObservation.h"
#include "cores/VideoPlayer/AudioSyncAcquisition.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

#include <gtest/gtest.h>

using namespace std::chrono_literals;

namespace
{
constexpr double TOLERANCE = 1e-8;

class ManualTimer
{
public:
  static inline std::chrono::milliseconds now{0};
  void Set(std::chrono::milliseconds interval)
  {
    m_start = now;
    m_interval = interval;
  }
  bool IsTimePast() const { return now - m_start >= m_interval; }
  std::chrono::steady_clock::time_point GetStartTime() const
  {
    return std::chrono::steady_clock::time_point(
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(m_start));
  }

private:
  std::chrono::milliseconds m_start{0};
  std::chrono::milliseconds m_interval{0};
};

using Error = ActiveAE::CSyncError<ManualTimer>;

unsigned int NativeStamp(std::chrono::milliseconds time)
{
  return static_cast<unsigned int>(
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(time).count());
}

TEST(AudioSyncWindow, AWindowPublishesOnItsDeadlineAndNotBefore)
{
  ManualTimer::now = 0ms;
  Error control;
  control.Add(54.0);

  double error = -1.0;
  ManualTimer::now = 99ms;
  EXPECT_FALSE(control.Get(error)) << "window published before its deadline";
  ManualTimer::now = 100ms;
  ASSERT_TRUE(control.Get(error)) << "window missed its deadline";
  EXPECT_NEAR(54.0, error, TOLERANCE);
}

TEST(AudioSyncWindow, ALatchedWindowKeepsItsValueAndDeadline)
{
  ManualTimer::now = 1ms;
  Error raw;
  raw.Add(120.0);

  double unused;
  EXPECT_FALSE(raw.Get(unused)) << "offset timer unexpectedly reached its deadline";

  // A latch stamps the moment it latches, so the clock has to be there first.
  ManualTimer::now = 100ms;
  raw.Latch(100ms);

  unsigned int stamp;
  EXPECT_NEAR(120.0, raw.GetLastError(stamp), TOLERANCE);
  EXPECT_EQ(NativeStamp(100ms), stamp) << "paired latch lost its deadline";
  EXPECT_TRUE(raw.LastErrorValid()) << "paired latch lost validity";
}

TEST(AudioSyncWindow, AnEngineCreditAdjustsTheValueWithoutStartingANewWindow)
{
  ManualTimer::now = 0ms;
  Error control;
  ManualTimer::now = 1ms;
  Error raw;
  control.Add(54.0);
  raw.Add(120.0);
  double error;
  ManualTimer::now = 100ms;
  ASSERT_TRUE(control.Get(error));
  raw.Latch(100ms);

  control.Add(9.0);
  raw.Add(20.0);
  control.Correction(-9.0);
  raw.Correction(-20.0);

  unsigned int stamp;
  EXPECT_NEAR(45.0, control.GetLastError(stamp), TOLERANCE);
  EXPECT_NEAR(100.0, raw.GetLastError(stamp), TOLERANCE);
  EXPECT_EQ(NativeStamp(100ms), stamp) << "a credit changed the window identity";

  ManualTimer::now = 200ms;
  ASSERT_TRUE(control.Get(error)) << "the next window did not publish";
  raw.Latch(100ms);
  EXPECT_NEAR(9.0, error, TOLERANCE);
  EXPECT_NEAR(20.0, raw.GetLastError(stamp), TOLERANCE);
}

TEST(AudioSyncWindow, AnEmptyWindowIsDistinctFromAMeasuredZero)
{
  ManualTimer::now = 0ms;
  Error control;
  double error;
  ManualTimer::now = 100ms;
  ASSERT_TRUE(control.Get(error)) << "empty window did not expire";
  EXPECT_NEAR(0.0, error, TOLERANCE);
  EXPECT_FALSE(control.LastErrorValid()) << "empty window reported a measured zero";

  control.Add(0.0);
  ManualTimer::now = 200ms;
  EXPECT_TRUE(control.Get(error));
  EXPECT_TRUE(control.LastErrorValid()) << "a measured zero became an empty window";
}

TEST(AudioSyncWindow, FlushDropsValidityAndRearmsTheDeadline)
{
  ManualTimer::now = 0ms;
  Error control;
  control.Add(999.0);
  control.Flush(1000ms);
  EXPECT_FALSE(control.LastErrorValid()) << "flush retained old validity";

  control.Add(10.0);
  double error;
  ManualTimer::now = 999ms;
  EXPECT_FALSE(control.Get(error)) << "flush did not rearm its deadline";
  ManualTimer::now = 1000ms;
  ASSERT_TRUE(control.Get(error)) << "flushed window did not publish";
  EXPECT_NEAR(10.0, error, TOLERANCE);
}

TEST(AudioSyncWindow, WideningTheIntervalDiscardsThePartialWindowButKeepsTheLastValue)
{
  ManualTimer::now = 0ms;
  Error control;
  control.Add(10.0);
  double error;
  ManualTimer::now = 100ms;
  ASSERT_TRUE(control.Get(error));

  control.Add(999.0);
  control.SetErrorInterval(6000ms);
  unsigned int stamp;
  EXPECT_NEAR(10.0, control.GetLastError(stamp), TOLERANCE) << "widening changed the last value";

  control.Add(12.0);
  ManualTimer::now = 6100ms;
  ASSERT_TRUE(control.Get(error)) << "widened window did not publish";
  EXPECT_NEAR(12.0, error, TOLERANCE);
}

TEST(AudioSyncWindow, TheLegacyTimestampStillTruncatesTheSameWay)
{
  // The published stamp is an unsigned int of native clock ticks. Past the
  // wrap point it truncates; that is existing behaviour and is pinned, not
  // corrected, because the consumer compares stamps for equality only.
  ManualTimer::now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::duration(
                             std::numeric_limits<unsigned int>::max())) +
                     100ms;
  Error control;
  control.Flush();
  unsigned int stamp;
  control.GetLastError(stamp);
  EXPECT_EQ(NativeStamp(ManualTimer::now), stamp) << "legacy timestamp truncation changed";
}

TEST(AudioSyncAcquisition, EqualReadingsCountOnceAndThreeAgreeingOnesSettle)
{
  CAudioSyncAcquisition acquisition;
  for (int packet = 0; packet < 10000; ++packet)
    acquisition.Observe(true, DVD_MSEC_TO_TIME(15.0));
  EXPECT_EQ(1, acquisition.Readings()) << "repeated identical readings were counted separately";
  EXPECT_FALSE(acquisition.Settled());

  acquisition.Observe(true, DVD_MSEC_TO_TIME(15.1));
  EXPECT_FALSE(acquisition.Settled()) << "two readings must not settle";
  acquisition.Observe(true, DVD_MSEC_TO_TIME(14.9));
  EXPECT_TRUE(acquisition.Settled()) << "three agreeing readings must settle";
}

TEST(AudioSyncAcquisition, TheMagnitudeWindowIsStrictAtBothEnds)
{
  CAudioSyncAcquisition acquisition;
  for (double residue : {15.0, 15.1, 14.9})
    acquisition.Observe(true, DVD_MSEC_TO_TIME(residue));
  ASSERT_TRUE(acquisition.Settled());
  EXPECT_TRUE(acquisition.WithinMagnitude(DVD_MSEC_TO_TIME(14.9))) << "settled residue rejected";

  for (double boundary : {0.0, 5.0, -5.0, 500.0, -500.0})
    EXPECT_FALSE(acquisition.WithinMagnitude(DVD_MSEC_TO_TIME(boundary)))
        << "boundary " << boundary << " should be excluded";
  for (double accepted : {5.001, -5.001, 499.999, -499.999})
    EXPECT_TRUE(acquisition.WithinMagnitude(DVD_MSEC_TO_TIME(accepted)))
        << "value " << accepted << " should be accepted";
}

TEST(AudioSyncAcquisition, AnInvalidReadingChangesNothingAndResetClearsEverything)
{
  CAudioSyncAcquisition acquisition;
  for (double residue : {15.0, 15.1, 14.9})
    acquisition.Observe(true, DVD_MSEC_TO_TIME(residue));

  acquisition.Observe(false, DVD_MSEC_TO_TIME(200.0));
  EXPECT_EQ(3, acquisition.Readings()) << "an invalid reading was counted";
  EXPECT_TRUE(acquisition.Settled()) << "an invalid reading undid settling";

  acquisition.Reset();
  EXPECT_EQ(0, acquisition.Readings());
  EXPECT_FALSE(acquisition.Settled()) << "a new acquisition inherited settling";
}

TEST(AudioSyncAcquisition, ReadingsThatKeepMovingExpireInsteadOfSettling)
{
  CAudioSyncAcquisition acquisition;
  for (double ramp : {20.0, 40.0, 60.0, 80.0, 100.0, 120.0, 140.0})
    acquisition.Observe(true, DVD_MSEC_TO_TIME(ramp));
  EXPECT_FALSE(acquisition.Settled()) << "a moving ramp settled";
  EXPECT_FALSE(acquisition.ReadingsExpired()) << "the ramp expired early";

  acquisition.Observe(true, DVD_MSEC_TO_TIME(160.0));
  EXPECT_TRUE(acquisition.ReadingsExpired()) << "eight changed readings did not expire";
}

TEST(AudioSyncAcquisition, TheThirtySecondTimeoutBoundaryIsExclusive)
{
  EXPECT_FALSE(CAudioSyncAcquisition::TimeExpired(DVD_SEC_TO_TIME(30.0)));
  EXPECT_TRUE(CAudioSyncAcquisition::TimeExpired(DVD_SEC_TO_TIME(30.0) + 1.0));
}

TEST(AudioSyncAcquisition, StationarityToleratesFractionsButNotTwoMilliseconds)
{
  CAudioSyncAcquisition acquisition;
  for (double jitter : {150.0, 155.0, 151.0})
    acquisition.Observe(true, DVD_MSEC_TO_TIME(jitter));
  EXPECT_TRUE(acquisition.Settled()) << "fractional stationarity tolerance changed";

  acquisition.Reset();
  for (double step : {10.0, 12.0, 14.0})
    acquisition.Observe(true, DVD_MSEC_TO_TIME(step));
  EXPECT_FALSE(acquisition.Settled()) << "the strict 2ms stationarity boundary changed";
}

// All three pieces together, driven the way a resume drives them.
class ScriptedResume : public testing::Test
{
protected:
  static constexpr double SCALE = 0.45;

  void SetUp() override { ManualTimer::now = 0ms; }

  void Sample(double error)
  {
    const double clippedRaw = std::clamp(error, -1000.0, 1000.0);
    const double clippedControl = std::clamp(error * SCALE, -1000.0, 1000.0);
    control.Add(clippedControl);
    raw.Add(clippedRaw);
    observations.Add(error, clippedRaw, clippedControl, SCALE, 1000.0, 10000.0 + error, 10000.0,
                     200.0, 1,
                     std::chrono::duration_cast<std::chrono::microseconds>(ManualTimer::now).count());
  }

  void Publish()
  {
    double error;
    ASSERT_TRUE(control.Get(error, 1000ms)) << "script missed a window publication";
    raw.Latch(1000ms);
    observations.Latch(19);
    unsigned int stamp;
    ASSERT_NEAR(error, observations.Last().control, TOLERANCE)
        << "observation and window disagree about the control error";
    ASSERT_NEAR(raw.GetLastError(stamp), observations.Last().raw, TOLERANCE)
        << "observation and window disagree about the raw error";
    acquisition.Observe(raw.LastErrorValid(), DVD_MSEC_TO_TIME(raw.GetLastError(stamp)));
  }

  Error control, raw;
  CAESyncObservations observations;
  CAudioSyncAcquisition acquisition;
};

TEST_F(ScriptedResume, ASaturatedFirstWindowSurvivesToPublication)
{
  Sample(1200.0);
  for (int i = 1; i < 10; ++i)
    Sample(0.0);
  ManualTimer::now = 100ms;
  Publish();

  EXPECT_TRUE(observations.Last().saturated) << "resume lost saturation before the last sample";
  EXPECT_NEAR(120.0, observations.Last().measured, TOLERANCE);
  EXPECT_NEAR(100.0, observations.Last().raw, TOLERANCE);
  EXPECT_NEAR(54.0, observations.Last().control, TOLERANCE);
}

TEST_F(ScriptedResume, ThreeAgreeingWindowsSettleAndTheResidueIsEligible)
{
  for (double residue : {15.0, 15.1, 14.9})
  {
    Sample(residue);
    ManualTimer::now += 1000ms;
    Publish();
  }
  EXPECT_TRUE(acquisition.Settled()) << "resume residue did not settle after three windows";
  EXPECT_TRUE(acquisition.WithinMagnitude(DVD_MSEC_TO_TIME(observations.Last().raw)))
      << "settled resume residue was not eligible by magnitude";
}

TEST_F(ScriptedResume, CreditingTheEngineDoesNotManufactureAMeasurement)
{
  for (double residue : {15.0, 15.1, 14.9})
  {
    Sample(residue);
    ManualTimer::now += 1000ms;
    Publish();
  }
  const auto immutable = observations.Last();

  control.Correction(-immutable.raw * SCALE);
  raw.Correction(-immutable.raw);

  unsigned int stamp;
  EXPECT_NEAR(0.0, raw.GetLastError(stamp), TOLERANCE);
  EXPECT_NEAR(0.0, control.GetLastError(stamp), TOLERANCE);
  EXPECT_NEAR(14.9, observations.Last().raw, TOLERANCE) << "credit rewrote the observation";
  EXPECT_TRUE(observations.Last().SameWindow(immutable)) << "credit manufactured a new measurement";

  acquisition.Reset();
  EXPECT_FALSE(acquisition.Settled()) << "a seek inherited old readiness";
}
} // namespace
