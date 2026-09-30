/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  The HDR10+ to Dolby Vision derivation, tested against the real production
 *  translation unit.
 *
 *  libdovi is a built dependency and the reason this was thought untestable.
 *  It does not have to be: no mocking framework can hook a C function, but the
 *  linker can. The test binary defines libdovi's six entry points itself, and
 *  because the converter hands its whole derivation to dovi_generate_from_json
 *  as text, defining that one function makes every derived value observable.
 *  Nothing in production changes.
 */

#include "cores/VideoPlayer/DVDCodecs/Video/AMLHdr10PlusToDv.h"

#include "cores/VideoPlayer/DVDCodecs/Video/AMLHdr10Plus.h"
#include "utils/BitstreamReader.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Hdr10PlusPayload.h"

extern "C"
{
#include <libdovi/rpu_parser.h>
}

// libdovi's opaque handle. The real one is a Rust object; here it need only be
// distinguishable, because the test answers different bytes for each.
struct DoviRpuOpaque
{
  int index;
};

namespace
{
std::string g_json;
int g_generateCalls = 0;
bool g_failGeneration = false;
int g_failWriteIndex = -1;
bool g_tagRpuWithPeak = false;
std::vector<uint8_t> g_taggedBytes[2];
DoviData g_taggedData[2]{};

DoviRpuOpaque g_rpus[2] = {{0}, {1}};
const DoviRpuOpaque* g_handles[2] = {&g_rpus[0], &g_rpus[1]};
DoviRpuOpaqueList g_list{};

// Distinguishable so a test can tell the scene-refresh RPU from the hold one.
const uint8_t g_refreshBytes[] = {0x7C, 0x01, 0xAA};
const uint8_t g_holdBytes[] = {0x7C, 0x01, 0xBB, 0xBB};
DoviData g_refresh{g_refreshBytes, sizeof(g_refreshBytes)};
DoviData g_hold{g_holdBytes, sizeof(g_holdBytes)};

void ResetLibdovi()
{
  g_json.clear();
  g_generateCalls = 0;
  g_failGeneration = false;
  g_failWriteIndex = -1;
  g_tagRpuWithPeak = false;
}

// A static metadata set the converter considers plausible, so the CMv4.0 path
// is reachable: BT.2020 primaries, D65 white, 1000 nit peak.
KODI::AML::HDR::HDRStaticMetadataInfo Static()
{
  KODI::AML::HDR::HDRStaticMetadataInfo info;
  info.max_lum = 1000;
  info.min_lum = 50;
  info.max_cll = 1000;
  info.max_fall = 400;
  info.display_primaries_x[0] = 8500;  // G
  info.display_primaries_y[0] = 39850;
  info.display_primaries_x[1] = 6550;  // B
  info.display_primaries_y[1] = 2300;
  info.display_primaries_x[2] = 35400; // R
  info.display_primaries_y[2] = 14600;
  info.white_point_x = 15635;
  info.white_point_y = 16450;
  info.has_mdcv = true;
  return info;
}

KODI::AML::HDR::Hdr10PlusMetadata Metadata(uint32_t maxscl = 1000)
{
  auto payload = yacer::WellFormed(1, maxscl);
  CBitstreamReader reader(payload.data(), static_cast<int>(payload.size()));
  const auto parsed = KODI::AML::HDR::hdr10plus_sei_to_metadata(reader);
  EXPECT_TRUE(parsed.has_value());
  return parsed.value_or(KODI::AML::HDR::Hdr10PlusMetadata{});
}

// Read one integer field out of the JSON the derivation produced.
long Field(const std::string& name)
{
  const std::string key = "\"" + name + "\":";
  const size_t position = g_json.find(key);
  if (position == std::string::npos)
    return -1;
  long value = -1;
  const auto result = std::from_chars(g_json.data() + position + key.size(),
                                     g_json.data() + g_json.size(), value);
  return result.ec == std::errc{} ? value : -1;
}

// Build escaped SEI NALs, including mixed messages and repeated T.35 payloads.
std::vector<uint8_t> Sei(
    const std::vector<std::pair<uint8_t, std::vector<uint8_t>>>& messages)
{
  std::vector<uint8_t> rbsp{0x4E, 0x01};
  for (const auto& [type, payload] : messages)
  {
    rbsp.push_back(type);
    size_t size = payload.size();
    while (size >= 255)
    {
      rbsp.push_back(255);
      size -= 255;
    }
    rbsp.push_back(static_cast<uint8_t>(size));
    rbsp.insert(rbsp.end(), payload.begin(), payload.end());
  }
  rbsp.push_back(0x80);

  std::vector<uint8_t> escaped;
  unsigned int zeros = 0;
  for (uint8_t byte : rbsp)
  {
    if (zeros == 2 && byte <= 3)
    {
      escaped.push_back(3);
      zeros = 0;
    }
    escaped.push_back(byte);
    zeros = byte == 0 ? zeros + 1 : 0;
  }
  return escaped;
}

const std::vector<uint8_t> kSlice{0x26, 0x01, 0x90, 0xAB};
const std::vector<uint8_t> kRefresh(std::begin(g_refreshBytes), std::end(g_refreshBytes));
// A recognized HDR10+ header with no body: it must be removed on fallback too.
const std::vector<uint8_t> kTruncatedHdr10Plus{0xB5, 0, 0x3C, 0, 1, 4, 0};

std::vector<uint8_t> AccessUnit(const std::vector<std::vector<uint8_t>>& nals)
{
  std::vector<uint8_t> au;
  for (const auto& nal : nals)
  {
    if (au.empty() || nal[0] == 0x7C)
      au.push_back(0);
    au.insert(au.end(), {0, 0, 1});
    au.insert(au.end(), nal.begin(), nal.end());
  }
  return au;
}

std::vector<uint8_t> HdrAu(uint32_t maxscl = 1000)
{
  return AccessUnit({Sei({{4, yacer::WellFormed(1, maxscl)}}), kSlice});
}

class Hdr10PlusSession : public testing::Test
{
protected:
  void SetUp() override
  {
    ResetLibdovi();
    session.SetStaticMetadata(Static());
  }

  bool Process(const std::vector<uint8_t>& au)
  {
    return session.ProcessAccessUnit(au.data(), static_cast<int>(au.size()), output);
  }

  KODI::AML::HDR::CHdr10PlusToDvSession session;
  std::vector<uint8_t> output;
};
} // namespace

extern "C"
{
const DoviRpuOpaqueList* dovi_generate_from_json(const char* json)
{
  g_json = json ? json : "";
  ++g_generateCalls;
  if (g_failGeneration)
    return nullptr;
  if (g_tagRpuWithPeak)
  {
    const long peak = Field("max_pq");
    for (int index = 0; index < 2; ++index)
    {
      // Encode the actual derived peak, with no accidental Annex-B start code.
      auto& bytes = g_taggedBytes[index];
      bytes = {0x7C, 0x01, static_cast<uint8_t>(index == 0 ? 0xAA : 0xBB)};
      for (int shift : {8, 4, 0})
        bytes.push_back(static_cast<uint8_t>(0xA0 | ((peak >> shift) & 0xF)));
      g_taggedData[index] = {bytes.data(), bytes.size()};
    }
  }
  g_list.list = g_handles;
  g_list.len = 2;
  g_list.error = nullptr;
  return &g_list;
}

const DoviData* dovi_write_unspec62_nalu(const DoviRpuOpaque* rpu)
{
  if (rpu->index == g_failWriteIndex)
    return nullptr;
  if (g_tagRpuWithPeak)
    return &g_taggedData[rpu->index];
  return rpu == &g_rpus[0] ? &g_refresh : &g_hold;
}

void dovi_data_free(const DoviData*)
{
}

const char* dovi_rpu_get_error(const DoviRpuOpaque*)
{
  return nullptr;
}

void dovi_rpu_list_free(const DoviRpuOpaqueList*)
{
}
}

using namespace KODI::AML::HDR;

TEST(Hdr10PlusToDv, GeneratesTheSceneRefreshRpuForNewMetadata)
{
  ResetLibdovi();
  Hdr10PlusRpuCache cache;
  const auto rpu = create_rpu_nalu_for_hdr10plus(cache, Metadata(), Static(), DvCmMode::V40);

  EXPECT_EQ(g_generateCalls, 1);
  EXPECT_FALSE(rpu.empty());
  // The serialized RPU refreshes its own picture.
  EXPECT_EQ(rpu, std::vector<uint8_t>(std::begin(g_refreshBytes), std::end(g_refreshBytes)));
}

TEST(Hdr10PlusToDv, AsksLibdoviForTheProfileAndVersionItWasToldTo)
{
  ResetLibdovi();
  Hdr10PlusRpuCache cache;
  create_rpu_nalu_for_hdr10plus(cache, Metadata(), Static(), DvCmMode::V29);
  EXPECT_NE(g_json.find("\"profile\":\"8.1\""), std::string::npos);
  EXPECT_NE(g_json.find("\"cm_version\":\"V29\""), std::string::npos);

  ResetLibdovi();
  Hdr10PlusRpuCache other;
  create_rpu_nalu_for_hdr10plus(other, Metadata(), Static(), DvCmMode::V40);
  EXPECT_NE(g_json.find("\"cm_version\":\"V40\""), std::string::npos);
}

TEST(Hdr10PlusToDv, CarriesTheStaticMetadataThroughAsLevel6)
{
  ResetLibdovi();
  Hdr10PlusRpuCache cache;
  create_rpu_nalu_for_hdr10plus(cache, Metadata(), Static(), DvCmMode::V40);
  EXPECT_EQ(Field("max_content_light_level"), 1000);
  EXPECT_EQ(Field("max_frame_average_light_level"), 400);
}

TEST(Hdr10PlusToDv, DerivesABrighterPeakFromBrighterContent)
{
  // The level 1 peak comes from the content's own maxscl, so more light in
  // must mean a higher peak out. This is the derivation the project argued
  // over and settled; it had no executable check until now.
  ResetLibdovi();
  Hdr10PlusRpuCache dim;
  create_rpu_nalu_for_hdr10plus(dim, Metadata(500), Static(), DvCmMode::V40);
  const long dimPeak = Field("max_pq");

  ResetLibdovi();
  Hdr10PlusRpuCache bright;
  create_rpu_nalu_for_hdr10plus(bright, Metadata(60000), Static(), DvCmMode::V40);
  const long brightPeak = Field("max_pq");

  EXPECT_GT(dimPeak, -1);
  EXPECT_GT(brightPeak, dimPeak);
}

TEST(Hdr10PlusToDv, AddsTheCmv40OnlyBlocksOnlyForV40)
{
  ResetLibdovi();
  Hdr10PlusRpuCache v29;
  create_rpu_nalu_for_hdr10plus(v29, Metadata(), Static(), DvCmMode::V29);
  // Level 3 and level 9 are CMv4.0 constructs; a V29 stream must not carry them.
  EXPECT_EQ(g_json.find("Level3"), std::string::npos);
  EXPECT_EQ(g_json.find("Level9"), std::string::npos);

  ResetLibdovi();
  Hdr10PlusRpuCache v40;
  create_rpu_nalu_for_hdr10plus(v40, Metadata(), Static(), DvCmMode::V40);
  EXPECT_NE(g_json.find("Level3"), std::string::npos);
  EXPECT_NE(g_json.find("Level9"), std::string::npos);
}

TEST(Hdr10PlusToDv, ReusesTheRefreshSerializationWhenTheMetadataIsUnchanged)
{
  ResetLibdovi();
  Hdr10PlusRpuCache cache;
  const auto meta = Metadata();
  const auto first = create_rpu_nalu_for_hdr10plus(cache, meta, Static(), DvCmMode::V40);
  ASSERT_EQ(g_generateCalls, 1);

  // Every picture refreshes, even if identical statistics reuse cached bytes.
  const auto second = create_rpu_nalu_for_hdr10plus(cache, meta, Static(), DvCmMode::V40);
  EXPECT_EQ(g_generateCalls, 1);
  EXPECT_EQ(second, first);
  EXPECT_NE(g_json.find("\"long_play_mode\":true"), std::string::npos);
  EXPECT_EQ(Field("length"), 1);

}

TEST_F(Hdr10PlusSession, MissingMetadataUsesTheCachedRefresh)
{
  ASSERT_TRUE(Process(HdrAu()));
  ASSERT_EQ(output, AccessUnit({kSlice, kRefresh}));
  for (int i = 0; i < 3; ++i)
  {
    ASSERT_TRUE(Process(AccessUnit({kSlice})));
    EXPECT_EQ(output, AccessUnit({kSlice, kRefresh}));
  }
  EXPECT_EQ(g_generateCalls, 1);
}

TEST_F(Hdr10PlusSession, RejectedMetadataIsRemovedAndUsesTheCachedRefresh)
{
  ASSERT_TRUE(Process(HdrAu()));
  ASSERT_TRUE(Process(AccessUnit({Sei({{4, kTruncatedHdr10Plus}}), kSlice})));
  EXPECT_EQ(output, AccessUnit({kSlice, kRefresh}));
}

TEST_F(Hdr10PlusSession, GenerationFailureUsesCachedMetadataThenRetries)
{
  ASSERT_TRUE(Process(HdrAu()));
  g_failGeneration = true;
  ASSERT_TRUE(Process(HdrAu(60000)));
  EXPECT_EQ(output, AccessUnit({kSlice, kRefresh}));
  g_failGeneration = false;
  ASSERT_TRUE(Process(HdrAu(60000)));
  EXPECT_EQ(output, AccessUnit({kSlice, kRefresh}));
  EXPECT_EQ(g_generateCalls, 3);
}

TEST_F(Hdr10PlusSession, SerializationFailureDoesNotReplaceTheCachedRpu)
{
  ASSERT_TRUE(Process(HdrAu()));
  g_failWriteIndex = 0;
  ASSERT_TRUE(Process(HdrAu(60000)));
  EXPECT_EQ(output, AccessUnit({kSlice, kRefresh}));
  g_failWriteIndex = -1;
  ASSERT_TRUE(Process(HdrAu(60000)));
  EXPECT_EQ(output, AccessUnit({kSlice, kRefresh}));
}

TEST_F(Hdr10PlusSession, SeekWithMissingMetadataStillRefreshesEveryPicture)
{
  ASSERT_TRUE(Process(HdrAu()));
  ASSERT_TRUE(Process(HdrAu()));
  ASSERT_EQ(output, AccessUnit({kSlice, kRefresh}));
  session.Reset();
  ASSERT_TRUE(Process(AccessUnit({kSlice})));
  EXPECT_EQ(output, AccessUnit({kSlice, kRefresh}));
  ASSERT_TRUE(Process(AccessUnit({kSlice})));
  EXPECT_EQ(output, AccessUnit({kSlice, kRefresh}));
}

TEST_F(Hdr10PlusSession, SeekWithRejectedMetadataStillRefreshes)
{
  ASSERT_TRUE(Process(HdrAu()));
  ASSERT_TRUE(Process(HdrAu()));
  session.Reset();
  ASSERT_TRUE(Process(AccessUnit({Sei({{4, kTruncatedHdr10Plus}}), kSlice})));
  EXPECT_EQ(output, AccessUnit({kSlice, kRefresh}));
}

TEST_F(Hdr10PlusSession, SeekRefreshSurvivesRepeatedDecoderRejections)
{
  ASSERT_TRUE(Process(HdrAu()));
  ASSERT_TRUE(Process(HdrAu()));
  session.Reset();
  for (int i = 0; i < 3; ++i)
  {
    ASSERT_TRUE(Process(AccessUnit({kSlice})));
    EXPECT_EQ(output, AccessUnit({kSlice, kRefresh}));
    session.RollbackAu();
  }
  ASSERT_TRUE(Process(AccessUnit({kSlice})));
  EXPECT_EQ(output, AccessUnit({kSlice, kRefresh}));
  ASSERT_TRUE(Process(AccessUnit({kSlice})));
  EXPECT_EQ(output, AccessUnit({kSlice, kRefresh}));
}

TEST_F(Hdr10PlusSession, SeekGenerationFailureStillRefreshesTheCachedMetadata)
{
  ASSERT_TRUE(Process(HdrAu()));
  ASSERT_TRUE(Process(HdrAu()));
  session.Reset();
  g_failGeneration = true;
  ASSERT_TRUE(Process(HdrAu(60000)));
  EXPECT_EQ(output, AccessUnit({kSlice, kRefresh}));
  ASSERT_TRUE(Process(HdrAu(60000)));
  EXPECT_EQ(output, AccessUnit({kSlice, kRefresh}));
}

TEST_F(Hdr10PlusSession, DecoderRetryPreservesEachPicturesRpu)
{
  for (const auto& [au, rpu] :
       std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>>{
           {HdrAu(), kRefresh}, {HdrAu(), kRefresh}, {HdrAu(60000), kRefresh}})
  {
    ASSERT_TRUE(Process(au));
    EXPECT_EQ(output, AccessUnit({kSlice, rpu}));
    session.RollbackAu();
    ASSERT_TRUE(Process(au));
    EXPECT_EQ(output, AccessUnit({kSlice, rpu}));
  }
}

TEST_F(Hdr10PlusSession, ColdCacheLeavesUnconvertibleAccessUnitsAlone)
{
  output = {0xA5};
  EXPECT_FALSE(Process(AccessUnit({kSlice})));
  EXPECT_FALSE(Process(AccessUnit({Sei({{4, kTruncatedHdr10Plus}}), kSlice})));
  g_failGeneration = true;
  EXPECT_FALSE(Process(HdrAu()));
  EXPECT_FALSE(session.Converted());
  EXPECT_FALSE(session.ConvertedThisAu());
  EXPECT_EQ(output, std::vector<uint8_t>{0xA5});
}

TEST_F(Hdr10PlusSession, NativeRpuOrEnhancementLayerDisablesConversionForTheStream)
{
  for (uint8_t type : {uint8_t{0x7C}, uint8_t{0x7E}})
  {
    SCOPED_TRACE(static_cast<int>(type));
    session = {};
    const std::vector<uint8_t> native{type, 0x01, 0xFE, 0xED};
    // Native metadata follows the HDR10+ SEI: inspect the entire AU first.
    output = {0xA5};
    EXPECT_FALSE(Process(AccessUnit({Sei({{4, yacer::WellFormed()}}), kSlice, native})));
    EXPECT_EQ(output, std::vector<uint8_t>{0xA5});
    EXPECT_FALSE(Process(HdrAu()));
    session.Reset();
    EXPECT_FALSE(Process(HdrAu()));
    EXPECT_FALSE(session.ConvertedThisAu());
  }
  EXPECT_EQ(g_generateCalls, 0);
}

TEST_F(Hdr10PlusSession, LateNativeRpuIsPreservedAndStopsFurtherConversion)
{
  ASSERT_TRUE(Process(HdrAu()));
  const std::vector<uint8_t> native{0x7C, 0x01, 0xFE, 0xED};
  EXPECT_FALSE(Process(AccessUnit({Sei({{4, yacer::WellFormed()}}), kSlice, native})));
  EXPECT_FALSE(session.ConvertedThisAu());
  EXPECT_FALSE(Process(HdrAu()));
  EXPECT_EQ(g_generateCalls, 1);
}

TEST_F(Hdr10PlusSession, RemovesEveryHdr10PlusMessageFromOneNal)
{
  const auto payload = yacer::WellFormed();
  ASSERT_TRUE(Process(AccessUnit({Sei({{4, payload}, {4, payload}}), kSlice})));
  EXPECT_EQ(output, AccessUnit({kSlice, kRefresh}));
}

TEST_F(Hdr10PlusSession, RemovingRepeatedMessagesPreservesOtherSeisAndSlices)
{
  const auto payload = yacer::WellFormed();
  const std::pair<uint8_t, std::vector<uint8_t>> cll{144, {0x03, 0xE8, 0, 0xC8}};
  const std::pair<uint8_t, std::vector<uint8_t>> other{5, {0, 0, 1, 0x20, 0, 0, 3}};
  auto differentProvider = payload;
  differentProvider[2] = 0x40;
  const std::pair<uint8_t, std::vector<uint8_t>> t35{4, differentProvider};
  const auto mixed = Sei({cll, {4, payload}, other, {4, payload}, t35});
  const auto remaining = Sei({cll, other, t35});
  ASSERT_TRUE(Process(AccessUnit({mixed, Sei({{4, payload}}), kSlice})));
  EXPECT_EQ(output, AccessUnit({remaining, kSlice, kRefresh}));
}

TEST_F(Hdr10PlusSession, RemovesEveryRejectedMessageWhenUsingCachedMetadata)
{
  ASSERT_TRUE(Process(HdrAu()));
  ASSERT_TRUE(Process(AccessUnit(
      {Sei({{4, kTruncatedHdr10Plus}, {4, kTruncatedHdr10Plus}}), kSlice})));
  EXPECT_EQ(output, AccessUnit({kSlice, kRefresh}));
}

TEST_F(Hdr10PlusSession, ReorderedPicturesKeepTheirOwnRpuAndAllSlices)
{
  g_tagRpuWithPeak = true;
  // IDs represent presentation order; packets arrive in decode order.
  // This tests AU ownership, not the hardware's subsequent display scheduling.
  for (uint8_t picture : {0, 3, 1, 2, 6, 4, 5})
  {
    SCOPED_TRACE(static_cast<int>(picture));
    const bool bright = picture % 2;
    const auto metadata = Sei({{4, yacer::WellFormed(1, bright ? 10000 : 1000)}});
    const std::vector<uint8_t> firstSlice{0x02, 0x01, 0x80, picture, 0xAB};
    const std::vector<uint8_t> secondSlice{0x02, 0x01, 0x40, picture, 0xCD};
    // ST 2084 quantized to 12 bits: 100 nits = 2081, 1000 nits = 3079.
    const int peak = bright ? 3079 : 2081;
    std::vector<uint8_t> expectedRpu{0x7C, 0x01, 0xAA};
    for (int shift : {8, 4, 0})
      expectedRpu.push_back(static_cast<uint8_t>(0xA0 | ((peak >> shift) & 0xF)));
    const auto au = AccessUnit({metadata, firstSlice, secondSlice});
    const auto expected = AccessUnit({firstSlice, secondSlice, expectedRpu});
    ASSERT_TRUE(Process(au));
    EXPECT_EQ(output, expected);
    // Decoder backpressure must preserve both the picture and its RPU.
    session.RollbackAu();
    ASSERT_TRUE(Process(au));
    EXPECT_EQ(output, expected);
  }
}

TEST_F(Hdr10PlusSession, RpuPrecedesEndOfSequenceAndEndOfBitstream)
{
  const std::vector<uint8_t> eos{0x48, 0x01, 0x80};
  const std::vector<uint8_t> eob{0x4A, 0x01, 0x80};
  const auto metadata = Sei({{4, yacer::WellFormed()}});
  ASSERT_TRUE(Process(AccessUnit({metadata, kSlice, eos, eob})));
  EXPECT_EQ(output, AccessUnit({kSlice, kRefresh, eos, eob}));
  ASSERT_TRUE(Process(AccessUnit({metadata, kSlice, eob})));
  EXPECT_EQ(output, AccessUnit({kSlice, kRefresh, eob}));
}

TEST_F(Hdr10PlusSession, NonPicturePacketsDoNotEmitAnRpu)
{
  ASSERT_TRUE(Process(HdrAu()));
  session.Reset();
  const std::vector<uint8_t> eos{0x48, 0x01, 0x80};
  output = {0xA5};
  EXPECT_FALSE(Process(AccessUnit({eos})));
  EXPECT_EQ(output, std::vector<uint8_t>{0xA5});
  EXPECT_FALSE(session.ConvertedThisAu());
  EXPECT_FALSE(Process(AccessUnit({Sei({{4, yacer::WellFormed()}})})));
  ASSERT_TRUE(Process(HdrAu()));
  EXPECT_EQ(output, AccessUnit({kSlice, kRefresh}));
}

TEST_F(Hdr10PlusSession, ReorderedSceneStartsRefreshTheirFirstDisplayedPicture)
{
  // Scene A: picture 0. Scene B: pictures 1,2,3. Decode the forward reference
  // picture 3 before B pictures 1 and 2. Dropping/repeating a displayed picture
  // must not make any other picture depend on it to activate its metadata.
  for (int picture : {0, 3, 1, 2})
  {
    SCOPED_TRACE(picture);
    ASSERT_TRUE(Process(HdrAu(picture == 0 ? 1000 : 10000)));
    EXPECT_EQ(output, AccessUnit({kSlice, kRefresh}));
  }
}
