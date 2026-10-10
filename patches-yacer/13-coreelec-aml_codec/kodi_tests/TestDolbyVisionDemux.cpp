/* SPDX-License-Identifier: GPL-2.0-or-later */
// Real adapter, HDR detection and pairing statements with controlled FFmpeg
// descriptors. The target build separately checks the production declarations.
#include "utils/DolbyVisionLegacy.h"

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <gtest/gtest.h>

namespace
{
enum { AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_HEVC, AV_PKT_DATA_DOVI_CONF,
       AV_PKT_DATA_MASTERING_DISPLAY_METADATA, AVCOL_TRC_SMPTE2084,
       AVCOL_TRC_ARIB_STD_B67, AVCOL_TRC_BT709, AVCOL_SPC_ICTCP, AVCOL_SPC_IPT_C2,
       AVCOL_SPC_UNSPECIFIED, AVCOL_SPC_BT2020_NCL, AVCOL_SPC_BT709, AVCOL_PRI_BT2020,
       AVCOL_PRI_BT709, AVCOL_RANGE_JPEG, AVCOL_RANGE_MPEG, AVDISCARD_ALL, LOGINFO, LOGDEBUG };
enum class StreamType { VIDEO };
enum class StreamHdrType { HDR_TYPE_NONE, HDR_TYPE_DOLBYVISION, HDR_TYPE_HDR10, HDR_TYPE_HLG };

struct AVDOVIDecoderConfigurationRecord
{
  int dv_version_major{}, dv_version_minor{}, dv_profile{}, dv_level{};
  int rpu_present_flag{}, el_present_flag{}, bl_present_flag{}, dv_bl_signal_compatibility_id{};
};
struct AVPacketSideData { int type; size_t size; const void* data; };
struct AVDictionaryEntry { const char* value; };
using AVDictionary = std::map<std::string, AVDictionaryEntry>;
struct AVCodecParameters
{
  int codec_type{AVMEDIA_TYPE_VIDEO}, codec_id{AV_CODEC_ID_HEVC};
  int width{}, height{}, color_trc{AVCOL_TRC_SMPTE2084};
  const AVPacketSideData* coded_side_data{};
  int nb_coded_side_data{};
};
struct AVStream
{
  int index{}, id{}, discard{};
  AVCodecParameters* codecpar{};
  const AVDictionary* metadata{};
};
struct AVInputFormat { const char* name; };
struct AVFormatContext
{
  const AVInputFormat* iformat;
  unsigned int nb_streams;
  AVStream** streams;
};
const AVDictionaryEntry* av_dict_get(const AVDictionary* dictionary, const char* key,
                                   const void*, int)
{
  const auto it = dictionary->find(key);
  return it == dictionary->end() ? nullptr : &it->second;
}
const AVPacketSideData* av_packet_side_data_get(const AVPacketSideData* data, int count, int type)
{
  for (int i = 0; i < count; ++i)
    if (data[i].type == type)
      return &data[i];
  return nullptr;
}
bool dvEnabled = true;
bool aml_dolby_vision_enabled() { return dvEnabled; }
// CoreELEC's VS10 / tone-mapping conversions (kodi 326cbc50 rewrites a stream's
// colour fields for them in AddStream); all off here.
bool aml_convert_to_dv_by_vs_engine(StreamHdrType) { return false; }
bool aml_convert_to_hdr_by_vs_engine(StreamHdrType) { return false; }
bool aml_convert_to_sdr_by_vs_engine(StreamHdrType) { return false; }
bool aml_convert_to_hdr(StreamHdrType) { return false; }
bool aml_convert_to_sdr(StreamHdrType) { return false; }
struct CLog
{
  template<typename... Args> static void Log(int, const char*, Args&&...) {}
};
struct CDemuxStream
{
  virtual ~CDemuxStream() = default;
  StreamType type{StreamType::VIDEO};
  int uniqueId{};
};
struct CDemuxStreamVideo : CDemuxStream
{
  StreamHdrType hdr_type{StreamHdrType::HDR_TYPE_NONE};
  int colorSpace{};
  int colorPrimaries{}, colorTransferCharacteristic{}, colorRange{};
  bool isDualStream{}, isELStream{};
  AVDOVIDecoderConfigurationRecord dovi;
};
struct CDVDDemuxFFmpeg
{
  AVFormatContext* m_pFormatContext;
  std::map<int, CDemuxStream*> m_streams;
  ~CDVDDemuxFFmpeg()
  {
    for (const auto& entry : m_streams)
      delete entry.second;
  }
  void RemoveStream(CDemuxStream* stream)
  {
    m_streams.erase(stream->uniqueId);
    delete stream;
  }
  StreamHdrType DetermineHdrType(AVStream* stream);
  CDemuxStream* AddPairingStream(int index);
};

#include "DoviDemuxSections.inc"

class DolbyVisionDemux : public testing::Test
{
protected:
  void SetUp() override { dvEnabled = true; }
  void TearDown() override { dvEnabled = true; }

  // Every permutation changes the actual container indices as well as AddStream
  // order: the ordinary video appears before, between and after the two layers.
  void CheckOrdering(std::array<int, 3> order, bool fullResolutionEl)
  {
    std::array<AVCodecParameters, 3> codecs;
    std::array<AVDictionary, 3> metadata;
    std::array<AVStream, 3> streams;
    std::array<AVStream*, 3> pointers;
    int base = -1, enhancement = -1, alternate = -1;
    for (int i = 0; i < 3; ++i)
    {
      const bool isBase = order[i] == 0;
      const bool isEnhancement = order[i] == 1;
      codecs[i].width = isBase || (isEnhancement && fullResolutionEl) ? 3840 : 1920;
      codecs[i].height = isBase || (isEnhancement && fullResolutionEl) ? 2160 : 1080;
      if (isBase)
      {
        base = i;
        metadata[i]["SOURCE_ID"] = {"001011"};
      }
      else if (isEnhancement)
      {
        enhancement = i;
        metadata[i]["SOURCE_ID-eng"] = {"001015"};
      }
      else
        alternate = i;
      streams[i] = {i, 0, 0, &codecs[i], &metadata[i]};
      pointers[i] = &streams[i];
    }
    AVInputFormat format{"matroska,webm"};
    AVFormatContext context{&format, 3, pointers.data()};
    CDVDDemuxFFmpeg demux{&context, {}};
    for (int i = 0; i < 3; ++i)
      ASSERT_NE(demux.AddPairingStream(i), nullptr);
    ASSERT_EQ(demux.m_streams.size(), 3u);
    const auto* bl = static_cast<CDemuxStreamVideo*>(demux.m_streams.at(base));
    const auto* el = static_cast<CDemuxStreamVideo*>(demux.m_streams.at(enhancement));
    const auto* other = static_cast<CDemuxStreamVideo*>(demux.m_streams.at(alternate));
    EXPECT_EQ(bl->hdr_type, StreamHdrType::HDR_TYPE_DOLBYVISION);
    EXPECT_TRUE(bl->isDualStream);
    EXPECT_FALSE(bl->isELStream);
    EXPECT_EQ(bl->dovi.dv_profile, 7);
    EXPECT_EQ(el->hdr_type, StreamHdrType::HDR_TYPE_DOLBYVISION);
    EXPECT_TRUE(el->isDualStream);
    EXPECT_TRUE(el->isELStream);
    EXPECT_EQ(other->hdr_type, StreamHdrType::HDR_TYPE_HDR10);
    EXPECT_FALSE(other->isDualStream);
    EXPECT_FALSE(other->isELStream);
  }
};

TEST_F(DolbyVisionDemux, HalfResolutionElStaysBoundWithAnExtraVideoInEveryOrder)
{
  std::array<int, 3> order{0, 1, 2};
  do
  {
    SCOPED_TRACE(testing::PrintToString(order));
    CheckOrdering(order, false);
  } while (std::next_permutation(order.begin(), order.end()));
}

TEST_F(DolbyVisionDemux, FullResolutionElStaysBoundWithAnExtraVideoInEveryOrder)
{
  std::array<int, 3> order{0, 1, 2};
  do
  {
    SCOPED_TRACE(testing::PrintToString(order));
    CheckOrdering(order, true);
  } while (std::next_permutation(order.begin(), order.end()));
}

TEST_F(DolbyVisionDemux, ContainerRecordTakesPriorityOverLegacyTags)
{
  const AVDOVIDecoderConfigurationRecord config{1, 0, 8, 6, 1, 0, 1, 1};
  const AVPacketSideData sideData{AV_PKT_DATA_DOVI_CONF, sizeof(config), &config};
  AVCodecParameters baseCodec, elCodec;
  baseCodec.width = 3840;
  baseCodec.height = 2160;
  elCodec.width = 1920;
  elCodec.height = 1080;
  elCodec.coded_side_data = &sideData;
  elCodec.nb_coded_side_data = 1;
  const AVDictionary baseMetadata{{"SOURCE_ID", {"001011"}}};
  const AVDictionary elMetadata{{"SOURCE_ID", {"001015"}}};
  AVStream base{0, 0, 0, &baseCodec, &baseMetadata};
  AVStream enhancement{1, 0, 0, &elCodec, &elMetadata};
  AVStream* streams[]{&base, &enhancement};
  AVInputFormat format{"matroska"};
  AVFormatContext context{&format, 2, streams};
  CDVDDemuxFFmpeg demux{&context, {}};
  ASSERT_NE(demux.AddPairingStream(0), nullptr);
  ASSERT_NE(demux.AddPairingStream(1), nullptr);
  EXPECT_EQ(static_cast<CDemuxStreamVideo*>(demux.m_streams.at(0))->dovi.dv_profile, 8);
}

TEST_F(DolbyVisionDemux, LegacyDetectionRequiresMatroskaAndEnabledDolbyVision)
{
  AVCodecParameters baseCodec, elCodec;
  baseCodec.width = 3840;
  baseCodec.height = 2160;
  elCodec.width = 1920;
  elCodec.height = 1080;
  const AVDictionary baseMetadata{{"SOURCE_ID", {"001011"}}};
  const AVDictionary elMetadata{{"SOURCE_ID", {"001015"}}};
  AVStream base{0, 0, 0, &baseCodec, &baseMetadata};
  AVStream enhancement{1, 0, 0, &elCodec, &elMetadata};
  AVStream* streams[]{&base, &enhancement};
  AVInputFormat format{"mpegts"};
  AVFormatContext context{&format, 2, streams};
  CDVDDemuxFFmpeg demux{&context, {}};
  EXPECT_EQ(demux.DetermineHdrType(&enhancement), StreamHdrType::HDR_TYPE_HDR10);
  format.name = "matroska";
  dvEnabled = false;
  EXPECT_EQ(demux.DetermineHdrType(&enhancement), StreamHdrType::HDR_TYPE_HDR10);
}
} // namespace
