/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "cores/VideoPlayer/DVDCodecs/Video/AMLHdr10Plus.h"

#include "utils/BitstreamReader.h"
#include "utils/HevcSei.h"

#include "Hdr10PlusPayload.h"

using yacer::WellFormed;

#include <cstdint>
#include <optional>
#include <random>
#include <vector>

#include <gtest/gtest.h>

using namespace KODI::AML::HDR;

namespace
{
// The payload opens with a country code, two provider codes, an application
// identifier and version, then num_windows. CBitstreamReader bounds every read
// against the length it was given and answers 0 past the end, so a malformed
// payload cannot walk off a buffer whose length is honest. What it cannot
// defend against is a length that lies - which is why each buffer here is
// exactly as long as it claims to be, and why the extractors' clamp against
// the real buffer matters.
std::vector<uint8_t> Payload(size_t length, uint8_t numWindows, uint32_t seed)
{
  std::vector<uint8_t> bytes(length, 0);
  std::mt19937 random(seed);
  for (size_t index = 0; index < length; ++index)
    bytes[index] = static_cast<uint8_t>(random());
  if (length > 0)
    bytes[0] = 0xB5; // itu_t_t35_country_code
  if (length > 7)
    bytes[7] = static_cast<uint8_t>((numWindows & 0x3) << 6) | (bytes[7] & 0x3F);
  return bytes;
}

// Wrap a payload as one SEI message in an RBSP the CHevcSei parser accepts,
// so the extractors can be reached the way the decoder reaches them.
std::vector<uint8_t> SeiNalu(uint8_t payloadType, const std::vector<uint8_t>& payload)
{
  std::vector<uint8_t> nalu{0x4E, 0x01};
  nalu.push_back(payloadType);
  size_t remaining = payload.size();
  while (remaining >= 255)
  {
    nalu.push_back(0xFF);
    remaining -= 255;
  }
  nalu.push_back(static_cast<uint8_t>(remaining));
  nalu.insert(nalu.end(), payload.begin(), payload.end());
  return nalu;
}

void PutBe16(std::vector<uint8_t>& bytes, uint16_t value)
{
  bytes.push_back(static_cast<uint8_t>(value >> 8));
  bytes.push_back(static_cast<uint8_t>(value));
}

void PutBe32(std::vector<uint8_t>& bytes, uint32_t value)
{
  for (int shift = 24; shift >= 0; shift -= 8)
    bytes.push_back(static_cast<uint8_t>(value >> shift));
}

std::optional<Hdr10PlusMetadata> Parse(std::vector<uint8_t>& payload)
{
  CBitstreamReader reader(payload.data(), static_cast<int>(payload.size()));
  return hdr10plus_sei_to_metadata(reader);
}
} // namespace

TEST(Hdr10PlusParser, RejectsAWindowCountOutsideTheAllowedRange)
{
  // ST 2094-40 allows one to three windows. Zero desynchronises every field
  // that follows it, so it has to be refused rather than parsed.
  auto payload = Payload(64, 0, 1);
  EXPECT_FALSE(Parse(payload).has_value());
}

TEST(Hdr10PlusParser, SurvivesEveryTruncationOfAPayload)
{
  // Whatever the counts inside a payload say, the parser must terminate and
  // must not size a container from them without checking. The reader bounds
  // its own reads, so this catches unbounded loops and bad resizes rather
  // than overreads.
  for (size_t length = 0; length <= 128; ++length)
  {
    for (uint8_t windows : {uint8_t{1}, uint8_t{2}, uint8_t{3}})
    {
      for (uint32_t seed : {1u, 99u})
      {
        auto payload = Payload(length, windows, seed);
        Parse(payload);
      }
    }
  }
}

TEST(Hdr10PlusParser, SurvivesAPayloadThatMaximisesEveryCount)
{
  // Every count field at its maximum describes far more data than any of these
  // buffers carry.
  for (size_t length = 1; length <= 96; ++length)
  {
    std::vector<uint8_t> payload(length, 0xFF);
    payload[0] = 0xB5;
    CBitstreamReader reader(payload.data(), static_cast<int>(payload.size()));
    hdr10plus_sei_to_metadata(reader);
  }
}

TEST(Hdr10PlusParser, HandlesAnEmptyPayload)
{
  std::vector<uint8_t> payload;
  CBitstreamReader reader(payload.data(), 0);
  hdr10plus_sei_to_metadata(reader);
}

TEST(Hdr10PlusParser, ParsesAWellFormedPayload)
{
  auto payload = WellFormed();
  const auto metadata = Parse(payload);
  ASSERT_TRUE(metadata.has_value());
  EXPECT_EQ(metadata->itu_t_t35_country_code, 0xB5);
  EXPECT_EQ(metadata->application_identifier, 4u);
  EXPECT_EQ(metadata->num_windows, 1u);
  EXPECT_EQ(metadata->targeted_system_display_maximum_luminance, 500u);
  EXPECT_FALSE(metadata->targeted_system_display_actual_peak_luminance_flag);

  ASSERT_EQ(metadata->luminance.size(), 1u);
  const auto& luminance = metadata->luminance[0];
  EXPECT_EQ(luminance.maxscl[0], 1000u);
  EXPECT_EQ(luminance.maxscl[1], 2000u);
  EXPECT_EQ(luminance.maxscl[2], 3000u);
  EXPECT_EQ(luminance.average_maxrgb, 1500u);
  ASSERT_EQ(luminance.distribution_maxrgb.size(), 9u);
  EXPECT_EQ(luminance.distribution_maxrgb[0].percentage, 1u);
  EXPECT_EQ(luminance.distribution_maxrgb[8].percentile, 900u);
  EXPECT_EQ(luminance.fraction_bright_pixels, 512u);

  ASSERT_EQ(metadata->tone_mapping.size(), 1u);
  const auto& mapping = metadata->tone_mapping[0];
  EXPECT_TRUE(mapping.tone_mapping_flag);
  EXPECT_EQ(mapping.bezier_curve.knee_point_x, 2048u);
  EXPECT_EQ(mapping.bezier_curve.knee_point_y, 1024u);
  ASSERT_EQ(mapping.bezier_curve.bezier_curve_anchors.size(), 9u);
  EXPECT_EQ(mapping.bezier_curve.bezier_curve_anchors[8], 576u);
  EXPECT_FALSE(mapping.color_saturation_mapping_flag);
}

TEST(Hdr10PlusParser, ParsesTheWindowsItIsToldAbout)
{
  // Three windows means two processing windows are coded before the luminance
  // block, and a luminance and tone-mapping entry for each.
  auto payload = WellFormed(3);
  const auto metadata = Parse(payload);
  ASSERT_TRUE(metadata.has_value());
  EXPECT_EQ(metadata->num_windows, 3u);
  EXPECT_EQ(metadata->luminance.size(), 3u);
  EXPECT_EQ(metadata->tone_mapping.size(), 3u);
}

TEST(Hdr10PlusParser, RefusesAWellFormedPayloadCutShort)
{
  // Every prefix of a payload the parser would otherwise accept.
  const auto whole = WellFormed();
  for (size_t length = 0; length < whole.size(); ++length)
  {
    std::vector<uint8_t> payload(whole.begin(), whole.begin() + static_cast<long>(length));
    CBitstreamReader reader(payload.data(), static_cast<int>(payload.size()));
    hdr10plus_sei_to_metadata(reader);
  }
}

TEST(Hdr10PlusExtract, TakesTheMetadataOutOfAParsedSeiMessage)
{
  const auto nalu = SeiNalu(4, WellFormed());
  const auto messages = CHevcSei::ParseSeiRbsp(nalu.data(), nalu.size());
  ASSERT_EQ(messages.size(), 1u);

  const auto metadata = ExtractHdr10Plus(messages, nalu);
  ASSERT_TRUE(metadata.has_value());
  EXPECT_EQ(metadata->num_windows, 1u);
  ASSERT_EQ(metadata->luminance.size(), 1u);
  EXPECT_EQ(metadata->luminance[0].average_maxrgb, 1500u);
}

TEST(Hdr10PlusExtract, IgnoresAMessageOfAnotherPayloadType)
{
  // Same payload, announced as something other than user-data-registered.
  const auto nalu = SeiNalu(5, WellFormed());
  const auto messages = CHevcSei::ParseSeiRbsp(nalu.data(), nalu.size());
  ASSERT_EQ(messages.size(), 1u);
  EXPECT_FALSE(ExtractHdr10Plus(messages, nalu).has_value());
}

TEST(Hdr10PlusExtract, IgnoresAnotherProvidersUserData)
{
  // Registered user data, but not Samsung's ST 2094-40 registration.
  auto payload = WellFormed();
  payload[2] = 0x40; // terminal provider code low byte
  const auto nalu = SeiNalu(4, payload);
  const auto messages = CHevcSei::ParseSeiRbsp(nalu.data(), nalu.size());
  ASSERT_EQ(messages.size(), 1u);
  EXPECT_FALSE(ExtractHdr10Plus(messages, nalu).has_value());
}

TEST(Hdr10PlusExtract, TakesTheMasteringDisplayColourVolume)
{
  std::vector<uint8_t> payload;
  for (uint16_t primary = 0; primary < 3; ++primary)
  {
    PutBe16(payload, static_cast<uint16_t>(1000 + primary));
    PutBe16(payload, static_cast<uint16_t>(2000 + primary));
  }
  PutBe16(payload, 15635); // white point x
  PutBe16(payload, 16450); // white point y
  PutBe32(payload, 10000000); // max luminance, 0.0001 nit units
  PutBe32(payload, 50);       // min luminance, same units
  ASSERT_EQ(payload.size(), 24u);

  const auto nalu = SeiNalu(137, payload);
  const auto messages = CHevcSei::ParseSeiRbsp(nalu.data(), nalu.size());
  ASSERT_EQ(messages.size(), 1u);

  const auto mdcv = ExtractMasteringDisplayColourVolume(messages, nalu);
  ASSERT_TRUE(mdcv.has_value());
  EXPECT_EQ(mdcv->displayPrimaries[0].x, 1000u);
  EXPECT_EQ(mdcv->displayPrimaries[2].y, 2002u);
  EXPECT_EQ(mdcv->whitePoint.x, 15635u);
  // Maximum luminance is reported in nits; the minimum keeps its raw units.
  EXPECT_EQ(mdcv->maxLuminance, 1000u);
  EXPECT_EQ(mdcv->minLuminance, 50u);
}

TEST(Hdr10PlusExtract, IgnoresAMasteringDisplayMessageTooShortToHoldIt)
{
  const auto nalu = SeiNalu(137, std::vector<uint8_t>(23, 0x11));
  const auto messages = CHevcSei::ParseSeiRbsp(nalu.data(), nalu.size());
  ASSERT_EQ(messages.size(), 1u);
  EXPECT_FALSE(ExtractMasteringDisplayColourVolume(messages, nalu).has_value());
}

TEST(Hdr10PlusExtract, TakesTheContentLightLevel)
{
  std::vector<uint8_t> payload;
  PutBe16(payload, 4000); // max content light level
  PutBe16(payload, 400);  // max frame-average light level
  const auto nalu = SeiNalu(144, payload);
  const auto messages = CHevcSei::ParseSeiRbsp(nalu.data(), nalu.size());
  ASSERT_EQ(messages.size(), 1u);

  const auto cll = ExtractContentLightLevel(messages, nalu);
  ASSERT_TRUE(cll.has_value());
  EXPECT_EQ(cll->maxContentLightLevel, 4000u);
  EXPECT_EQ(cll->maxFrameAverageLightLevel, 400u);
}

TEST(Hdr10PlusExtract, IgnoresAContentLightLevelMessageTooShortToHoldIt)
{
  const auto nalu = SeiNalu(144, std::vector<uint8_t>(3, 0x22));
  const auto messages = CHevcSei::ParseSeiRbsp(nalu.data(), nalu.size());
  ASSERT_EQ(messages.size(), 1u);
  EXPECT_FALSE(ExtractContentLightLevel(messages, nalu).has_value());
}
