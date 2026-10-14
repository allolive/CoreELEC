/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "utils/DolbyVisionLegacy.h"

#include <gtest/gtest.h>

namespace
{
TEST(DolbyVisionLegacy, RecognizesATaggedUhdBaseAndHalfResolutionEnhancementInEitherOrder)
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
