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

#include <cstdint>
#include <regex>
#include <string>
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
  const std::regex pattern("\"" + name + "\":(-?[0-9]+)");
  std::smatch match;
  if (!std::regex_search(g_json, match, pattern))
    return -1;
  return std::stol(match[1]);
}
} // namespace

extern "C"
{
const DoviRpuOpaqueList* dovi_generate_from_json(const char* json)
{
  g_json = json ? json : "";
  ++g_generateCalls;
  g_list.list = g_handles;
  g_list.len = 2;
  g_list.error = nullptr;
  return &g_list;
}

const DoviData* dovi_write_unspec62_nalu(const DoviRpuOpaque* rpu)
{
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
  // The first access unit of a shot carries the refresh RPU, not the hold one.
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

TEST(Hdr10PlusToDv, HoldsInsteadOfRegeneratingWhenTheMetadataIsUnchanged)
{
  ResetLibdovi();
  Hdr10PlusRpuCache cache;
  const auto meta = Metadata();
  const auto first = create_rpu_nalu_for_hdr10plus(cache, meta, Static(), DvCmMode::V40);
  ASSERT_EQ(g_generateCalls, 1);

  // HDR10+ is piecewise constant within a scene, so the same metadata arrives
  // for many frames. Regenerating each time would signal a scene cut on every
  // one of them; the second call must answer the hold RPU from the cache.
  const auto second = create_rpu_nalu_for_hdr10plus(cache, meta, Static(), DvCmMode::V40);
  EXPECT_EQ(g_generateCalls, 1) << "regenerated for unchanged metadata";
  EXPECT_EQ(second, std::vector<uint8_t>(std::begin(g_holdBytes), std::end(g_holdBytes)));
  EXPECT_NE(first, second);
}
