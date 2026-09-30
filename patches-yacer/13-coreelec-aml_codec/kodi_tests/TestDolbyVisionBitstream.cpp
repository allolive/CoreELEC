/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "utils/BitstreamConverter.h"

#include <gtest/gtest.h>
#include <vector>

struct DoviRpuOpaque {};
namespace
{
using Bytes = std::vector<uint8_t>;
DoviRpuOpaque rpu;
DoviRpuDataHeader header{7, "MEL"};
const Bytes rewritten{0x7C, 1, 0xAA};
const DoviData serialized{rewritten.data(), rewritten.size()};
int convertedMode = -1;

const Bytes blSlice{0x26, 1, 0x80, 0xAB};
const Bytes elSlice{0x26, 1, 0x80, 0xCD};
const Bytes nativeRpu{0x7C, 1, 0xFA, 0xFB, 0xFC, 0xFD};
const Bytes eos{0x48, 1, 0x80};
const Bytes eob{0x4A, 1, 0x80};

Bytes Wrapped(const Bytes& nal)
{
  Bytes result{0x7E, 1};
  result.insert(result.end(), nal.begin(), nal.end());
  return result;
}

Bytes Pack(const std::vector<Bytes>& nals, bool lengthPrefixed)
{
  Bytes result;
  for (const auto& nal : nals)
  {
    if (lengthPrefixed)
    {
      const uint32_t size = nal.size();
      result.insert(result.end(), {uint8_t(size >> 24), uint8_t(size >> 16),
                                   uint8_t(size >> 8), uint8_t(size)});
    }
    else
    {
      if (result.empty() || nal[0] == 0x7C)
        result.push_back(0);
      result.insert(result.end(), {0, 0, 1});
    }
    result.insert(result.end(), nal.begin(), nal.end());
  }
  return result;
}

class Converter : public CBitstreamConverter
{
public:
  explicit Converter(bool lengthPrefixed)
  {
    m_to_annexb = true;
    m_convert_bitstream = lengthPrefixed;
    m_codec = AV_CODEC_ID_HEVC;
  }
  Bytes Output() const { return {GetConvertBuffer(), GetConvertBuffer() + GetConvertSize()}; }
};

TEST(DolbyVisionBitstream, FelAndMelKeepBothLayersAndTheRpuBeforeBothTerminators)
{
  for (const char* layer : {"FEL", "MEL"})
    for (bool lengthPrefixed : {false, true})
    {
      header.el_type = layer;
      Converter converter(lengthPrefixed);
      auto bl = Pack({blSlice, eos, eob}, lengthPrefixed);
      auto el = Pack({elSlice, nativeRpu, eos}, lengthPrefixed);
      ASSERT_TRUE(converter.Convert(bl.data(), bl.size(), el.data(), el.size()));
      EXPECT_EQ(converter.Output(), Pack({blSlice, Wrapped(elSlice), nativeRpu,
                                          Wrapped(eos), eos, eob}, false));
      EXPECT_EQ(converter.GetDoviIsFEL(), std::string_view(layer) == "FEL");
    }
}

TEST(DolbyVisionBitstream, ElTerminatorBeforeRpuIsDeferredUntilAfterItsMetadata)
{
  Converter converter(true);
  auto bl = Pack({blSlice, eos}, true);
  auto el = Pack({elSlice, eos, nativeRpu}, true);
  ASSERT_TRUE(converter.Convert(bl.data(), bl.size(), el.data(), el.size()));
  EXPECT_EQ(converter.Output(), Pack({blSlice, Wrapped(elSlice), nativeRpu, Wrapped(eos), eos}, false));
}

TEST(DolbyVisionBitstream, RewritingAnRpuCannotChangeTraversalOfTheInputPacket)
{
  for (bool convertTo8 : {false, true})
  {
    Converter converter(true);
    converter.SetDoviZeroLevel5(true);
    converter.SetConvertDovi(convertTo8);
    convertedMode = -1;
    auto bl = Pack({blSlice, eob}, true);
    auto el = Pack({elSlice, nativeRpu, elSlice, eos}, true);
    ASSERT_TRUE(converter.Convert(bl.data(), bl.size(), el.data(), el.size()));
    EXPECT_EQ(converter.Output(), convertTo8
        ? Pack({blSlice, rewritten, eob}, false)
        : Pack({blSlice, Wrapped(elSlice), rewritten, Wrapped(elSlice), Wrapped(eos), eob}, false));
    EXPECT_EQ(convertedMode, convertTo8 ? 2 : -1);
  }
}

TEST(DolbyVisionBitstream, SingleTrackProfileConversionPreservesRpuAndEndMarkers)
{
  Converter converter(false);
  converter.SetConvertDovi(true);
  auto au = Pack({blSlice, Wrapped(elSlice), eos, nativeRpu, eob}, false);
  ASSERT_TRUE(converter.Convert(au.data(), au.size()));
  EXPECT_EQ(converter.Output(), Pack({blSlice, rewritten, eos, eob}, false));
}
} // namespace

extern "C"
{
DoviRpuOpaque* dovi_parse_unspec62_nalu(const uint8_t*, size_t) { return &rpu; }
const DoviRpuDataHeader* dovi_rpu_get_header(const DoviRpuOpaque*) { return &header; }
int dovi_convert_rpu_with_mode(DoviRpuOpaque*, uint8_t mode) { convertedMode = mode; return 0; }
int dovi_rpu_set_active_area_offsets(DoviRpuOpaque*, uint16_t, uint16_t, uint16_t, uint16_t) { return 0; }
const DoviData* dovi_write_unspec62_nalu(const DoviRpuOpaque*) { return &serialized; }
void dovi_data_free(const DoviData*) {}
void dovi_rpu_free_header(const DoviRpuDataHeader*) {}
void dovi_rpu_free(DoviRpuOpaque*) {}
}
