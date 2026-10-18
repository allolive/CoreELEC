/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Blu-ray 3D subtitle depth: the OFMD table an MVC access unit opening a GOP
 *  carries in an SEI, and the offset each picture takes by its own pts. The
 *  decoder holds pictures between input and output, so a table counted from
 *  output pictures would change depth before the cut it belongs to.
 */

#include "cores/VideoPlayer/DVDCodecs/Video/AMLMvcSubtitleDepth.h"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

using namespace AML_MVC_DEPTH;

namespace
{
constexpr double FRAME = 41708.0; // 23.976 fps in DVD time

// an SEI NAL with one user_data_unregistered message holding the OFMD payload
std::vector<uint8_t> OfmdSei(const std::vector<std::vector<int>>& offsets)
{
  std::vector<uint8_t> payload = {0x17, 0xee, 0x8c, 0x60, 0xf8, 0x4d, 0x11, 0xd9,
                                  0x8c, 0xd6, 0x08, 0x00, 0x20, 0x0c, 0x9a, 0x66,
                                  'O',  'F',  'M',  'D',  0,    0,    0,    0,
                                  0,    0};
  payload.push_back(static_cast<uint8_t>(offsets.size()));
  payload.push_back(static_cast<uint8_t>(offsets.empty() ? 0 : offsets[0].size()));
  payload.push_back(0);
  payload.push_back(0);
  for (const auto& sequence : offsets)
    for (int v : sequence)
      payload.push_back(static_cast<uint8_t>(v < 0 ? 0x80 | -v : v));
  std::vector<uint8_t> rbsp = {0x05, static_cast<uint8_t>(payload.size())};
  rbsp.insert(rbsp.end(), payload.begin(), payload.end());
  rbsp.push_back(0x80);
  // emulation prevention, as an encoder writes it
  std::vector<uint8_t> nal = {0x06};
  int zeros = 0;
  for (uint8_t b : rbsp)
  {
    if (zeros >= 2 && b <= 3)
    {
      nal.push_back(0x03);
      zeros = 0;
    }
    nal.push_back(b);
    zeros = b == 0 ? zeros + 1 : 0;
  }
  return nal;
}

// the stream's escaping of an SEI's rbsp (header byte not included)
std::vector<uint8_t> Escaped(const std::vector<uint8_t>& rbsp)
{
  std::vector<uint8_t> nal = {0x06};
  int zeros = 0;
  for (uint8_t b : rbsp)
  {
    if (zeros >= 2 && b <= 3)
    {
      nal.push_back(0x03);
      zeros = 0;
    }
    nal.push_back(b);
    zeros = b == 0 ? zeros + 1 : 0;
  }
  return nal;
}

// an SEI payload size or type: 0xff per 255, then the rest
void PushSeiNumber(std::vector<uint8_t>& out, size_t value)
{
  for (; value >= 255; value -= 255)
    out.push_back(0xff);
  out.push_back(static_cast<uint8_t>(value));
}

// As an MVC Blu-ray remux carries it: the OFMD message nested in an MVC scalable
// nesting SEI (payload type 37), all 32 sequences of 24 frames at offset 6. The
// bytes up to the offsets are those of a real stream.
std::vector<uint8_t> NestedOfmdSei()
{
  std::vector<uint8_t> ofmd = {0x17, 0xee, 0x8c, 0x60, 0xf8, 0x4d, 0x11, 0xd9, 0x8c, 0xd6,
                               0x08, 0x00, 0x20, 0x0c, 0x9a, 0x66, 'O',  'F',  'M',  'D',
                               0x81, 0x00, 0x80, 0x1f, 0xff, 0xf0, 0xa0, 0x18, 0x80, 0x00};
  ofmd.insert(ofmd.end(), 32 * 24, 0x06);

  std::vector<uint8_t> nested = {0x40}; // all view components of the access unit
  PushSeiNumber(nested, 5);
  PushSeiNumber(nested, ofmd.size());
  nested.insert(nested.end(), ofmd.begin(), ofmd.end());

  std::vector<uint8_t> rbsp;
  PushSeiNumber(rbsp, 37);
  PushSeiNumber(rbsp, nested.size());
  rbsp.insert(rbsp.end(), nested.begin(), nested.end());
  rbsp.push_back(0x80);
  return Escaped(rbsp);
}

std::vector<uint8_t> LengthPrefixed(const std::vector<std::vector<uint8_t>>& nals)
{
  std::vector<uint8_t> au;
  for (const auto& nal : nals)
  {
    const size_t n = nal.size();
    au.insert(au.end(), {static_cast<uint8_t>(n >> 24), static_cast<uint8_t>(n >> 16),
                         static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n)});
    au.insert(au.end(), nal.begin(), nal.end());
  }
  return au;
}
} // namespace

TEST(MvcSubtitleDepth, ReadsSignedOffsetsPerSequence)
{
  const auto au = LengthPrefixed({{0x09, 0xf0}, OfmdSei({{3, -2, 0}, {-5, 7, 1}}), {0x65, 0x88}});
  Offsets offsets;
  ASSERT_TRUE(ParseOfmd(au.data(), au.size(), 4, offsets));
  ASSERT_EQ(offsets.size(), 2u);
  EXPECT_EQ(offsets[0], (std::vector<int8_t>{3, -2, 0}));
  EXPECT_EQ(offsets[1], (std::vector<int8_t>{-5, 7, 1}));
}

TEST(MvcSubtitleDepth, ReadsAnnexB)
{
  std::vector<uint8_t> au = {0, 0, 0, 1, 0x09, 0xf0, 0, 0, 1};
  const auto sei = OfmdSei({{4, 4}});
  au.insert(au.end(), sei.begin(), sei.end());
  au.insert(au.end(), {0, 0, 1, 0x65, 0x88});
  Offsets offsets;
  ASSERT_TRUE(ParseOfmd(au.data(), au.size(), 0, offsets));
  EXPECT_EQ(offsets[0], (std::vector<int8_t>{4, 4}));
}

TEST(MvcSubtitleDepth, UnescapesBeforeReading)
{
  // offsets 0, 0, 3 are written 00 00 03 03 in the stream
  const auto sei = OfmdSei({{0, 0, 3}});
  const auto au = LengthPrefixed({sei});
  Offsets offsets;
  ASSERT_TRUE(ParseOfmd(au.data(), au.size(), 4, offsets));
  EXPECT_EQ(offsets[0], (std::vector<int8_t>{0, 0, 3}));
}

TEST(MvcSubtitleDepth, RefusesTruncatedOrForeignPayloads)
{
  Offsets offsets;
  auto sei = OfmdSei({{1, 2, 3, 4}});
  sei.resize(sei.size() - 3); // the message runs past the NAL
  auto au = LengthPrefixed({sei});
  EXPECT_FALSE(ParseOfmd(au.data(), au.size(), 4, offsets));

  auto other = OfmdSei({{1}});
  other[3] ^= 0xff; // another uuid (its first byte, after the header and type and size)
  au = LengthPrefixed({other});
  EXPECT_FALSE(ParseOfmd(au.data(), au.size(), 4, offsets));

  const std::vector<uint8_t> bad = {0, 0, 0, 200, 0x06};
  EXPECT_FALSE(ParseOfmd(bad.data(), bad.size(), 4, offsets));
  EXPECT_TRUE(offsets.empty());
}

TEST(MvcSubtitleDepth, PicturesTakeTheirOwnFrame)
{
  CTables tables;
  tables.Add(10 * FRAME, {{1, 2, 3, 4}});
  tables.Add(14 * FRAME, {{-1, -2}});
  EXPECT_EQ(tables.Offset(10 * FRAME, 0, FRAME), 1);
  EXPECT_EQ(tables.Offset(12 * FRAME, 0, FRAME), 3);
  EXPECT_EQ(tables.Offset(13 * FRAME, 0, FRAME), 4);
  // the cut: the next GOP's first picture, not a few pictures early
  EXPECT_EQ(tables.Offset(14 * FRAME, 0, FRAME), -1);
  EXPECT_EQ(tables.Offset(15 * FRAME + 300, 0, FRAME), -2);
}

TEST(MvcSubtitleDepth, HoldsTheLastOffsetPastATablesEnd)
{
  CTables tables;
  tables.Add(0, {{5, 6}});
  EXPECT_EQ(tables.Offset(9 * FRAME, 0, FRAME), 6);
}

TEST(MvcSubtitleDepth, LeadingPicturesMoveTheGopStart)
{
  CTables tables;
  tables.Add(0, {{9, 9, 9}});
  // the next GOP opens with an I picture shown after two leading B pictures
  tables.Add(5 * FRAME, {{1, 2, 3, 4, 5}});
  tables.Fed(8 * FRAME);
  tables.Fed(3 * FRAME);
  tables.Fed(4 * FRAME);
  EXPECT_EQ(tables.Offset(3 * FRAME, 0, FRAME), 1);
  EXPECT_EQ(tables.Offset(5 * FRAME, 0, FRAME), 3);
  EXPECT_EQ(tables.Offset(2 * FRAME, 0, FRAME), 9);
}

TEST(MvcSubtitleDepth, NoTableNoSequenceNoOffset)
{
  CTables tables;
  EXPECT_FALSE(tables.Offset(0, 0, FRAME));
  tables.Add(10 * FRAME, {{1}});
  EXPECT_FALSE(tables.Offset(5 * FRAME, 0, FRAME));
  EXPECT_FALSE(tables.Offset(10 * FRAME, 1, FRAME));
  EXPECT_FALSE(tables.Offset(10 * FRAME, -1, FRAME));
  tables.Clear();
  EXPECT_FALSE(tables.Offset(10 * FRAME, 0, FRAME));
}

TEST(MvcSubtitleDepth, OnlyZerosAreNoDepth)
{
  CTables tables;
  tables.Add(0, {{0, 0}, {0, 3}});
  EXPECT_FALSE(tables.Offset(0, 0, FRAME));
  EXPECT_EQ(tables.Offset(0, 1, FRAME), 0);
  EXPECT_EQ(tables.Offset(FRAME, 1, FRAME), 3);
}

TEST(MvcSubtitleDepth, ADepthOnceSetHoldsThroughZerosAndSeeks)
{
  CTables tables;
  tables.Add(0, {{0, 0}});
  EXPECT_FALSE(tables.Offset(0, 0, FRAME));
  tables.Add(2 * FRAME, {{0, -4}});
  EXPECT_EQ(tables.Offset(2 * FRAME, 0, FRAME), 0);
  EXPECT_EQ(tables.Offset(3 * FRAME, 0, FRAME), -4);
  tables.Clear();
  tables.Add(100 * FRAME, {{0, 0}});
  EXPECT_EQ(tables.Offset(100 * FRAME, 0, FRAME), 0);
}

TEST(MvcSubtitleDepth, ReadsOfmdNestedInMvcScalableNesting)
{
  const auto sei = NestedOfmdSei();
  // the layout of the stream: 06 | 25 ff ff ff 27 | 40 | 05 ff ff ff 21 | uuid
  ASSERT_GE(sei.size(), 12u);
  EXPECT_EQ(sei[1], 0x25);
  EXPECT_EQ(sei[5], 0x27);
  EXPECT_EQ(sei[6], 0x40);
  EXPECT_EQ(sei[7], 0x05);
  EXPECT_EQ(sei[11], 0x21);

  const auto au = LengthPrefixed({{0x09, 0xf0}, sei, {0x65, 0x88}});
  Offsets offsets;
  ASSERT_TRUE(ParseOfmd(au.data(), au.size(), 4, offsets));
  ASSERT_EQ(offsets.size(), 32u);
  for (const auto& sequence : offsets)
    EXPECT_EQ(sequence, std::vector<int8_t>(24, 6));
}

TEST(MvcSubtitleDepth, BoundsANestedTableByItsPayload)
{
  auto sei = NestedOfmdSei();
  // the outer message claims one byte less than the nested table needs
  sei[5] = 0x26;
  const auto au = LengthPrefixed({sei});
  Offsets offsets;
  EXPECT_FALSE(ParseOfmd(au.data(), au.size(), 4, offsets));
}
