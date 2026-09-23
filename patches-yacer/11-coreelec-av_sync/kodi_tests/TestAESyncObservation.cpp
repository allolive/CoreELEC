/*
 *  Copyright (C) 2026 Team CoreELEC
 *  This file is part of CoreELEC - https://coreelec.org
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

// A window is both a measurement and an identity. The averages have to be
// right, but so does the identity: the player logs a window once, attributes
// an actuation to the window that caused it, and must never mistake a new
// window for one it has already seen.

#include "cores/AudioEngine/Utils/AESyncObservation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>

#include <gtest/gtest.h>

namespace
{
constexpr int STREAM = 7;
constexpr double TOLERANCE = 1e-9;

void Add(CAESyncObservations& observer, double error, int64_t now,
         double scale = 0.45, double clamp = 1000.0, uint64_t generation = 1)
{
  observer.Add(error, std::clamp(error, -clamp, clamp),
               std::clamp(error * scale, -clamp, clamp), scale, clamp,
               10000.0 + error, 10000.0, 200.0, generation, now);
}

// One saturated sample followed by unclipped ones: the window this produces is
// the fixture several cases below start from.
AESyncObservation Clipped(CAESyncObservations& observer)
{
  Add(observer, 1200.0, 200);
  for (int i = 1; i < 10; ++i)
    Add(observer, 0.0, 200 + i);
  observer.Latch(STREAM);
  return observer.Last();
}

TEST(AESyncObservation, AnEmptyWindowIsNotAMeasurementButStillConsumesAnIdentity)
{
  CAESyncObservations observer;
  observer.Latch(STREAM);
  ASSERT_FALSE(observer.Last().Valid()) << "empty window must not be a measurement";
  const auto emptySequence = observer.Last().sequence;

  Add(observer, 0.0, 100);
  observer.Latch(STREAM);
  EXPECT_TRUE(observer.Last().Valid()) << "a measured zero is a valid measurement";
  EXPECT_GT(observer.Last().sequence, emptySequence) << "empty window kept its identity";
  EXPECT_EQ(100, observer.Last().begin) << "sample timestamps must survive latching";
  EXPECT_EQ(100, observer.Last().end) << "sample timestamps must survive latching";
}

TEST(AESyncObservation, SaturationSurvivesLaterUnclippedSamples)
{
  CAESyncObservations observer;
  const auto clipped = Clipped(observer);

  EXPECT_NEAR(120.0, clipped.measured, TOLERANCE);
  EXPECT_NEAR(100.0, clipped.raw, TOLERANCE);
  EXPECT_NEAR(54.0, clipped.control, TOLERANCE);
  EXPECT_TRUE(clipped.saturated) << "early saturation did not survive an unclipped final sample";
  EXPECT_EQ(10u, clipped.samples) << "window lost samples";
  EXPECT_EQ(STREAM, clipped.stream) << "window lost provenance";
  EXPECT_EQ(1u, clipped.delayGeneration) << "window lost provenance";
}

TEST(AESyncObservation, ReadingAWindowRepeatedlyStillDescribesTheSameWindow)
{
  CAESyncObservations observer;
  const auto clipped = Clipped(observer);
  // The player polls this far more often than it latches; if a re-read ever
  // looked like a new window it would emit the same trace thousands of times.
  for (int packet = 0; packet < 10000; ++packet)
    ASSERT_TRUE(observer.Last().SameWindow(clipped)) << "re-read looked like a new window";
}

TEST(AESyncObservation, ActuatingDoesNotCreateAMeasurementAndIsAttributedToItsSource)
{
  CAESyncObservations observer;
  const auto clipped = Clipped(observer);

  observer.Insert(20.0);
  observer.Drop(20.0);
  observer.Mute(20.0);
  EXPECT_NEAR(clipped.measured, observer.Last().measured, TOLERANCE);
  EXPECT_NEAR(clipped.raw, observer.Last().raw, TOLERANCE);
  EXPECT_NEAR(clipped.control, observer.Last().control, TOLERANCE);
  EXPECT_EQ(clipped.sequence, observer.Last().sequence) << "actuation became a fresh measurement";
  EXPECT_EQ(clipped.sequence, observer.Actions().firstSequence) << "actuation lost its source";
  EXPECT_EQ(clipped.sequence, observer.Actions().lastSequence) << "actuation lost its source";

  observer.ClearActions();
  EXPECT_EQ(0u, observer.Actions().inserted) << "action report was not drained";
}

TEST(AESyncObservation, WindowsCarryingEqualValuesStillGetDistinctIdentities)
{
  CAESyncObservations observer;
  const auto clipped = Clipped(observer);

  for (int i = 0; i < 3; ++i)
  {
    const auto previous = observer.Last().sequence;
    Add(observer, 15.0, 300 + i);
    observer.Latch(STREAM);
    ASSERT_GT(observer.Last().sequence, previous) << "equal-valued windows shared an identity";
    ASSERT_FALSE(observer.Last().SameWindow(clipped)) << "new window mistaken for a cached one";
    ASSERT_NEAR(15.0, observer.Last().raw, TOLERANCE);
  }
}

TEST(AESyncObservation, FlushDropsEverythingAndDoesNotReuseAnIdentity)
{
  CAESyncObservations observer;
  Add(observer, 15.0, 300);
  observer.Latch(STREAM);
  const auto last = observer.Last();

  Add(observer, -900.0, 400);
  observer.Flush();
  EXPECT_FALSE(observer.Last().Valid()) << "flush retained a published measurement";
  EXPECT_EQ(0u, observer.Pending(STREAM).samples) << "flush retained partial samples";

  observer.Mute(20.0);
  EXPECT_EQ(0u, observer.Actions().firstSequence) << "post-flush mute blamed an obsolete window";
  EXPECT_EQ(0u, observer.Actions().lastSequence) << "post-flush mute blamed an obsolete window";
  observer.ClearActions();

  Add(observer, -12.0, 401);
  observer.Latch(STREAM);
  EXPECT_GT(observer.Last().epoch, last.epoch) << "flush reused a measurement identity";
  EXPECT_GT(observer.Last().sequence, last.sequence) << "flush reused a measurement identity";
  EXPECT_NEAR(-12.0, observer.Last().raw, TOLERANCE);
}

// A window whose samples were taken under different conditions cannot be
// averaged into one number, whatever the numbers look like.
TEST(AESyncObservation, ADelayResetInsideAWindowMakesItMixed)
{
  CAESyncObservations observer;
  Add(observer, 10.0, 500, 0.45, 1000.0, 1);
  Add(observer, 20.0, 501, 0.45, 1000.0, 2);
  observer.Latch(STREAM);
  EXPECT_TRUE(observer.Last().mixed) << "delay reset went undetected";
  EXPECT_FALSE(observer.Last().Valid()) << "a mixed window is not a measurement";
}

TEST(AESyncObservation, AScaleChangeInsideAWindowMakesItMixed)
{
  CAESyncObservations observer;
  Add(observer, 10.0, 510);
  Add(observer, 10.0, 511, 1.0);
  observer.Latch(STREAM);
  EXPECT_TRUE(observer.Last().mixed) << "scale change went undetected";
}

TEST(AESyncObservation, AClampChangeInsideAWindowMakesItMixed)
{
  CAESyncObservations observer;
  Add(observer, 10.0, 520);
  Add(observer, 10.0, 521, 0.45, 5000.0);
  observer.Latch(STREAM);
  EXPECT_TRUE(observer.Last().mixed) << "clamp change went undetected";
}

class AESyncObservationNonFinite : public testing::TestWithParam<double>
{
};

TEST_P(AESyncObservationNonFinite, ANonFiniteSampleDisqualifiesTheWindow)
{
  CAESyncObservations observer;
  Add(observer, GetParam(), 600);
  Add(observer, 0.0, 601);
  observer.Latch(STREAM);
  EXPECT_FALSE(observer.Last().finite) << "nonfinite sample was not noticed";
  EXPECT_FALSE(observer.Last().Valid()) << "nonfinite sample became a valid measurement";
}

INSTANTIATE_TEST_SUITE_P(EveryNonFiniteValue,
                         AESyncObservationNonFinite,
                         testing::Values(std::numeric_limits<double>::infinity(),
                                         -std::numeric_limits<double>::infinity(),
                                         std::numeric_limits<double>::quiet_NaN()));

TEST(AESyncObservation, ASumThatOverflowsIsNotAValidMeasurement)
{
  CAESyncObservations observer;
  Add(observer, std::numeric_limits<double>::max(), 610);
  Add(observer, std::numeric_limits<double>::max(), 611);
  observer.Latch(STREAM);
  EXPECT_FALSE(observer.Last().finite) << "overflowed sum became valid";
}

TEST(AESyncObservation, RestartingTheIntervalKeepsTheLastPublishedSnapshot)
{
  CAESyncObservations observer;
  Add(observer, 10.0, 700);
  observer.Latch(STREAM);
  const auto published = observer.Last();

  Add(observer, 100.0, 701);
  observer.RestartWindow();
  EXPECT_EQ(published.sequence, observer.Last().sequence) << "interval restart lost the snapshot";

  Add(observer, 20.0, 702);
  observer.Latch(STREAM);
  EXPECT_NEAR(20.0, observer.Last().raw, TOLERANCE) << "restart kept the discarded sample";
}

// The averages are the whole point, so they are checked against independently
// accumulated sums over random input rather than against hand-picked cases.
class AESyncObservationAverages
  : public testing::TestWithParam<std::pair<double, double>>
{
};

TEST_P(AESyncObservationAverages, TheLatchedAveragesMatchTheSamplesItWasGiven)
{
  const auto [scale, clamp] = GetParam();
  CAESyncObservations observer;
  std::mt19937 random(12345);
  std::uniform_real_distribution<double> errors(-20000.0, 20000.0);

  for (int window = 0; window < 1000; ++window)
  {
    observer.Flush();
    double raw = 0.0, control = 0.0, measured = 0.0;
    bool saturated = false;
    for (int sample = 0; sample < 50; ++sample)
    {
      const double error = errors(random);
      Add(observer, error, 800 + sample, scale, clamp);
      measured += error;
      raw += std::clamp(error, -clamp, clamp);
      control += std::clamp(error * scale, -clamp, clamp);
      saturated |= std::abs(error) >= clamp;
    }
    observer.Latch(STREAM);
    ASSERT_NEAR(measured / 50, observer.Last().measured, TOLERANCE) << "window " << window;
    ASSERT_NEAR(raw / 50, observer.Last().raw, TOLERANCE) << "window " << window;
    ASSERT_NEAR(control / 50, observer.Last().control, TOLERANCE) << "window " << window;
    ASSERT_EQ(saturated, observer.Last().saturated) << "window " << window;
  }
}

INSTANTIATE_TEST_SUITE_P(EveryScaleAndClamp,
                         AESyncObservationAverages,
                         testing::Values(std::pair{0.45, 1000.0},
                                         std::pair{0.45, 5000.0},
                                         std::pair{1.0, 1000.0},
                                         std::pair{1.0, 5000.0}));
} // namespace
