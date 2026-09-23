/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "utils/HevcSei.h"

#include <cstdint>
#include <initializer_list>
#include <vector>

#include <gtest/gtest.h>

namespace
{
// An SEI RBSP is two bytes of NAL header, which the parser skips, followed by
// messages. Each message codes a payload type and a payload size as any run of
// 0xFF bytes plus a final byte, then carries that many payload bytes.
std::vector<uint8_t> Rbsp(std::initializer_list<uint8_t> body)
{
  std::vector<uint8_t> buffer{0x4E, 0x01};
  buffer.insert(buffer.end(), body);
  return buffer;
}

std::vector<CHevcSei> Parse(const std::vector<uint8_t>& rbsp)
{
  return CHevcSei::ParseSeiRbsp(rbsp.data(), rbsp.size());
}
} // namespace

TEST(HevcSei, ParsesAWellFormedMessage)
{
  const auto messages = Parse(Rbsp({0x01, 0x02, 0xAA, 0xBB}));
  ASSERT_EQ(messages.size(), 1u);
  EXPECT_EQ(messages[0].m_payloadType, 1u);
  EXPECT_EQ(messages[0].m_payloadSize, 2u);
  // Offsets are byte positions into the RBSP, past the two header bytes.
  EXPECT_EQ(messages[0].m_msgOffset, 2u);
  EXPECT_EQ(messages[0].m_payloadOffset, 4u);
}

TEST(HevcSei, ParsesMessagesInSequence)
{
  const auto messages = Parse(Rbsp({0x01, 0x01, 0xAA, 0x02, 0x01, 0xBB}));
  ASSERT_EQ(messages.size(), 2u);
  EXPECT_EQ(messages[0].m_payloadType, 1u);
  EXPECT_EQ(messages[1].m_payloadType, 2u);
  EXPECT_EQ(messages[1].m_payloadOffset, 7u);
}

TEST(HevcSei, AccumulatesAnFfCodedTypeAndSize)
{
  // 0xFF then 0x01 is type 256; the size byte that follows is an ordinary 2.
  const auto messages = Parse(Rbsp({0xFF, 0x01, 0x02, 0xAA, 0xBB}));
  ASSERT_EQ(messages.size(), 1u);
  EXPECT_EQ(messages[0].m_payloadType, 256u);
  EXPECT_EQ(messages[0].m_payloadSize, 2u);
}

// The payload size is a count of bytes. Comparing it against a count of bits
// let a message declare up to eight times the data that remained, and the
// parser then skipped past the end of the buffer.
TEST(HevcSei, RefusesAPayloadLongerThanTheDataThatRemains)
{
  // Two payload bytes are present; the message claims five. Five is still
  // below the sixteen bits that remain, so a bits-against-bytes test admits it.
  EXPECT_TRUE(Parse(Rbsp({0x01, 0x05, 0xAA, 0xBB})).empty());
}

TEST(HevcSei, RefusesAPayloadOneByteTooLong)
{
  EXPECT_TRUE(Parse(Rbsp({0x01, 0x03, 0xAA, 0xBB})).empty());
}

TEST(HevcSei, AcceptsAPayloadThatExactlyFillsTheBuffer)
{
  EXPECT_EQ(Parse(Rbsp({0x01, 0x02, 0xAA, 0xBB})).size(), 1u);
}

TEST(HevcSei, KeepsTheMessagesBeforeAnOverlongOne)
{
  // The first message is whole; the second claims five bytes with one left.
  const auto messages = Parse(Rbsp({0x01, 0x01, 0xAA, 0x02, 0x05, 0xBB}));
  ASSERT_EQ(messages.size(), 1u);
  EXPECT_EQ(messages[0].m_payloadType, 1u);
}

TEST(HevcSei, RefusesAnFfCodedSizeBeyondTheBuffer)
{
  // 0xFF then 0x00 is a size of 255, against four bytes of payload.
  EXPECT_TRUE(Parse(Rbsp({0x01, 0xFF, 0x00, 0xAA, 0xBB, 0xCC, 0xDD})).empty());
}

TEST(HevcSei, IgnoresABufferTooShortToHoldAMessage)
{
  EXPECT_TRUE(Parse(Rbsp({0x01, 0x02})).empty());
}

// Without the saturating AvailableBits this input does not terminate. The
// payload-type run reads past the end, ReadBits advances the position anyway,
// the unclamped remainder wraps to about four billion, the size check passes,
// SkipBits(0) moves nothing and the loop never reaches its exit - messages
// accumulate until the process is killed. Verified: the unpatched reader hangs
// on exactly these five bytes.
TEST(HevcSei, TerminatesOnAPayloadTypeRunReachingTheEndOfTheBuffer)
{
  EXPECT_LE(Parse(Rbsp({0xFF, 0xFF, 0xFF})).size(), 1u);
}

TEST(HevcSei, StopsAtTheTrailingBitsAfterTheLastMessage)
{
  // A real RBSP ends in rbsp_trailing_bits - one 0x80 byte. The loop must stop
  // there rather than read it as another message header.
  EXPECT_EQ(Parse(Rbsp({0x01, 0x02, 0xAA, 0xBB, 0x80})).size(), 1u);
}
