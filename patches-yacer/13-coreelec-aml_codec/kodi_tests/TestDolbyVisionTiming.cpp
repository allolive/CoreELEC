/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cores/VideoPlayer/DVDCodecs/Video/AMLDolbyVisionTiming.h"
#include "utils/DolbyVisionLegacy.h"

#include <limits>
#include <gtest/gtest.h>

namespace
{
constexpr double NONE = -1e300;
constexpr double TOLERANCE = 2000;

TEST(DolbyVisionTiming, EitherArrivalOrderReturnsTheBaseLayerTimestamps)
{
  const AMLDoviTiming bl{42000, 30000};
  const AMLDoviTiming el{43000, 31000};
  for (bool elFirst : {false, true})
  {
    const auto match = AMLDoviMatchTiming(elFirst, elFirst ? el : bl,
                                         !elFirst, elFirst ? bl : el, NONE, TOLERANCE);
    ASSERT_TRUE(match);
    EXPECT_EQ(match->pts, bl.pts);
    EXPECT_EQ(match->dts, bl.dts);
  }
}

TEST(DolbyVisionTiming, MissingElPtsDoesNotDiscardTheBasePts)
{
  const auto match = AMLDoviMatchTiming(false, {42000, 30000}, true, {NONE, 30000},
                                       NONE, TOLERANCE);
  ASSERT_TRUE(match);
  EXPECT_EQ(match->pts, 42000);
}

TEST(DolbyVisionTiming, PresentationMismatchCannotBeOverriddenByDecodeTime)
{
  EXPECT_FALSE(AMLDoviMatchTiming(false, {42000, 30000}, true, {84000, 30000},
                                 NONE, TOLERANCE));
  EXPECT_FALSE(AMLDoviMatchTiming(false, {42000, 30000}, true, {44000, 30000},
                                 NONE, TOLERANCE));
}

TEST(DolbyVisionTiming, MissingPacketsCannotTurnIntoArrivalPairing)
{
  EXPECT_FALSE(AMLDoviMatchTiming(false, {NONE, NONE}, true, {NONE, NONE}, NONE, TOLERANCE));
  EXPECT_FALSE(AMLDoviMatchTiming(false, {42000, NONE}, true, {NONE, 30000}, NONE, TOLERANCE));
  EXPECT_FALSE(AMLDoviMatchTiming(false, {42000, 30000}, false, {42000, 30000}, NONE, TOLERANCE));
  const double nan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(AMLDoviMatchTiming(false, {nan, NONE}, true, {42000, NONE}, NONE, TOLERANCE));
}

TEST(DolbyVisionTiming, ReorderedPicturesMatchByPts)
{
  for (int picture : {0, 3, 1, 2})
  {
    const double pts = picture * 42000;
    ASSERT_TRUE(AMLDoviMatchTiming(false, {pts, NONE}, true, {pts, NONE}, NONE, TOLERANCE));
    EXPECT_FALSE(AMLDoviMatchTiming(false, {pts, NONE}, true, {pts + 42000, NONE},
                                   NONE, TOLERANCE));
  }
}

TEST(DolbyVisionLegacy, RecognizesTheTaggedBirdsOfPreyPairInEitherOrder)
{
  const DolbyVisionLegacyTrack bl{true, "001011", 3840, 2160};
  const DolbyVisionLegacyTrack el{true, "001015", 1920, 1080};
  for (bool elFirst : {false, true})
  {
    const auto layers = DolbyVisionLegacyLayers(elFirst ? std::vector{el, bl} : std::vector{bl, el});
    ASSERT_TRUE(layers);
    EXPECT_EQ(layers->base, elFirst ? 1u : 0u);
    EXPECT_EQ(layers->enhancement, elFirst ? 0u : 1u);
  }
  const auto layers = DolbyVisionLegacyLayers({{false, "", 600, 882}, bl, el});
  ASSERT_TRUE(layers);
  EXPECT_EQ(layers->base, 1u);
  EXPECT_EQ(layers->enhancement, 2u);
}

TEST(DolbyVisionLegacy, RequiresUniqueExplicitTagsAndCompatibleHevcPictures)
{
  const DolbyVisionLegacyTrack bl{true, "001011", 3840, 2160};
  const DolbyVisionLegacyTrack el{true, "001015", 1920, 1080};
  EXPECT_FALSE(DolbyVisionLegacyLayers({bl, {true, "", 1920, 1080}}));
  EXPECT_FALSE(DolbyVisionLegacyLayers({{true, "", 3840, 2160}, el}));
  EXPECT_FALSE(DolbyVisionLegacyLayers({bl, {false, "001015", 1920, 1080}}));
  EXPECT_FALSE(DolbyVisionLegacyLayers({bl, {true, "001015", 1280, 720}}));
  EXPECT_FALSE(DolbyVisionLegacyLayers({bl, bl, el}));
  EXPECT_FALSE(DolbyVisionLegacyLayers({bl, el, el}));
  EXPECT_FALSE(DolbyVisionLegacyLayers({el}));
  EXPECT_FALSE(DolbyVisionLegacyLayers({}));
}
} // namespace
