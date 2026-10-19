/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  MVC pictures now and then come out carrying each other's timestamps, the
 *  later one first. The codec holds one picture back and puts the labels back
 *  when the next is earlier; the pictures themselves go out in the order they
 *  came. The real Reorder/DropLate bodies and VideoPicture's reference
 *  handling are compiled in; the pictures and buffers around them stand in.
 */

#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "utils/log.h"

#include <cstdint>
#include <deque>
#include <optional>
#include <utility>

#include <gtest/gtest.h>

#define __MODULE_NAME__ "DVDVideoCodecAmlogic"

namespace
{
class CVideoBuffer
{
public:
  virtual ~CVideoBuffer() = default;
  void Acquire() { ++refs; }
  void Release()
  {
    if (--refs < 0)
      ADD_FAILURE() << "buffer released more often than acquired";
  }
  int refs{0};
};

class CAMLVideoBuffer : public CVideoBuffer
{
public:
  int id{};
  uint64_t m_omxPts{};
};

struct VideoPicture
{
  VideoPicture() = default;
  ~VideoPicture();
  VideoPicture& CopyRef(const VideoPicture& pic);
  void Reset()
  {
    if (videoBuffer)
      videoBuffer->Release();
    videoBuffer = nullptr;
    pts = DVD_NOPTS_VALUE;
    iFlags = 0;
    m_3dSubtitleDepth.reset();
  }

  CVideoBuffer* videoBuffer = nullptr;
  double pts{DVD_NOPTS_VALUE};
  unsigned int iFlags{0};
  std::optional<int> m_3dSubtitleDepth;
};

class CDVDVideoCodec
{
public:
  enum VCReturn
  {
    VC_NONE = 0,
    VC_ERROR,
    VC_FATAL,
    VC_BUFFER,
    VC_PICTURE,
    VC_FLUSHED,
    VC_NOBUFFER,
    VC_REOPEN,
    VC_EOF
  };
};

class CDVDVideoCodecAmlogic : public CDVDVideoCodec
{
public:
  VCReturn Reorder(VideoPicture* pVideoPicture, VCReturn retVal);
  void DropLate(VideoPicture* pVideoPicture);

  VideoPicture m_held;
  double m_lastOut{DVD_NOPTS_VALUE};
  int m_codecControlFlags{0};
  struct
  {
    int fpsrate{24000};
    int fpsscale{1001};
  } m_hints;
};

#include "MvcReorderSections.inc"

constexpr double FRAME = 41708.0;

uint64_t Omx(double pts)
{
  return static_cast<uint64_t>(pts * 9 / 100);
}

class MvcReorder : public testing::Test
{
protected:
  void TearDown() override
  {
    pic.Reset();
    codec.m_held.Reset();
    for (const auto& buffer : buffers)
      EXPECT_EQ(0, buffer.refs) << "picture " << buffer.id << " leaked or over-released";
  }

  // GetPicture's own steps: the previous picture's buffer goes back, the new
  // one comes from the pool with the picture's single reference.
  CDVDVideoCodec::VCReturn Decode(double pts,
                                  std::optional<int> depth = std::nullopt,
                                  unsigned int flags = 0)
  {
    if (pic.videoBuffer)
      pic.videoBuffer->Release();
    pic.videoBuffer = nullptr;
    pic.pts = pts;
    pic.iFlags = flags;
    pic.m_3dSubtitleDepth = depth;
    auto& buffer = buffers.emplace_back();
    buffer.id = static_cast<int>(buffers.size()) - 1;
    buffer.m_omxPts = Omx(pts);
    buffer.refs = 1;
    pic.videoBuffer = &buffer;
    const auto result = codec.Reorder(&pic, CDVDVideoCodec::VC_PICTURE);
    CheckOwners();
    return result;
  }

  CDVDVideoCodec::VCReturn Drain(CDVDVideoCodec::VCReturn decoder)
  {
    codec.m_codecControlFlags = DVD_CODEC_CTRL_DRAIN;
    const auto result = codec.Reorder(&pic, decoder);
    CheckOwners();
    return result;
  }

  // every buffer is owned once, by the picture out or the one held, or not at all
  void CheckOwners()
  {
    for (const auto& buffer : buffers)
    {
      const int owners = (pic.videoBuffer == &buffer) + (codec.m_held.videoBuffer == &buffer);
      EXPECT_EQ(owners, buffer.refs) << "picture " << buffer.id;
    }
  }

  const CAMLVideoBuffer& Out() const { return *static_cast<CAMLVideoBuffer*>(pic.videoBuffer); }
  bool Dropped() const { return (pic.iFlags & DVP_FLAG_DROPPED) != 0; }

  // destroyed last: the pictures release into them
  std::deque<CAMLVideoBuffer> buffers;
  CDVDVideoCodecAmlogic codec;
  VideoPicture pic;
};

TEST_F(MvcReorder, InOrderPicturesPassThroughOnePictureLate)
{
  EXPECT_EQ(CDVDVideoCodec::VC_BUFFER, Decode(0));
  EXPECT_EQ(nullptr, pic.videoBuffer) << "the held picture also went out";

  for (int i = 1; i < 5; ++i)
  {
    ASSERT_EQ(CDVDVideoCodec::VC_PICTURE, Decode(i * FRAME));
    EXPECT_EQ(i - 1, Out().id);
    EXPECT_EQ((i - 1) * FRAME, pic.pts);
    EXPECT_EQ(Omx((i - 1) * FRAME), Out().m_omxPts);
    EXPECT_FALSE(Dropped());
  }
}

TEST_F(MvcReorder, ASwappedPairGoesOutInArrivalOrderWithItsLabelsPutBack)
{
  Decode(0);
  ASSERT_EQ(CDVDVideoCodec::VC_PICTURE, Decode(2 * FRAME, 5));
  EXPECT_EQ(0, Out().id);

  ASSERT_EQ(CDVDVideoCodec::VC_PICTURE, Decode(FRAME, 3));
  EXPECT_EQ(1, Out().id) << "the pictures left their arrival order";
  EXPECT_EQ(FRAME, pic.pts);
  EXPECT_EQ(Omx(FRAME), Out().m_omxPts);
  EXPECT_EQ(3, pic.m_3dSubtitleDepth);
  EXPECT_FALSE(Dropped());

  ASSERT_EQ(CDVDVideoCodec::VC_PICTURE, Decode(3 * FRAME));
  EXPECT_EQ(2, Out().id);
  EXPECT_EQ(2 * FRAME, pic.pts);
  EXPECT_EQ(Omx(2 * FRAME), Out().m_omxPts);
  EXPECT_EQ(5, pic.m_3dSubtitleDepth);
  EXPECT_FALSE(Dropped());
}

TEST_F(MvcReorder, APictureOlderThanOneAlreadyOutIsDropped)
{
  Decode(0);
  Decode(2 * FRAME);
  Decode(3 * FRAME);
  EXPECT_EQ(2 * FRAME, pic.pts);

  // two frames back is no neighbour's label: it keeps its own
  ASSERT_EQ(CDVDVideoCodec::VC_PICTURE, Decode(FRAME));
  EXPECT_EQ(2, Out().id);
  EXPECT_EQ(3 * FRAME, pic.pts);
  EXPECT_FALSE(Dropped());

  ASSERT_EQ(CDVDVideoCodec::VC_PICTURE, Decode(4 * FRAME));
  EXPECT_EQ(3, Out().id);
  EXPECT_EQ(FRAME, pic.pts);
  EXPECT_TRUE(Dropped());
}

TEST_F(MvcReorder, AJumpBackOfASecondOrMoreIsADiscontinuityNotASwap)
{
  const double start = 10 * DVD_TIME_BASE;
  Decode(start);
  Decode(start + FRAME);

  ASSERT_EQ(CDVDVideoCodec::VC_PICTURE, Decode(start + FRAME - DVD_TIME_BASE));
  EXPECT_EQ(1, Out().id);
  EXPECT_EQ(start + FRAME, pic.pts) << "swapped across a discontinuity";
  EXPECT_EQ(Omx(start + FRAME), Out().m_omxPts);
  EXPECT_FALSE(Dropped());

  ASSERT_EQ(CDVDVideoCodec::VC_PICTURE, Decode(start + 2 * FRAME - DVD_TIME_BASE));
  EXPECT_EQ(2, Out().id);
  EXPECT_EQ(start + FRAME - DVD_TIME_BASE, pic.pts);
  EXPECT_FALSE(Dropped()) << "the order did not start again";

  ASSERT_EQ(CDVDVideoCodec::VC_PICTURE, Decode(start + 3 * FRAME - DVD_TIME_BASE));
  EXPECT_EQ(start + 2 * FRAME - DVD_TIME_BASE, pic.pts);
  EXPECT_FALSE(Dropped());
}

TEST_F(MvcReorder, AnOlderPictureIsNotSwappedButDropped)
{
  const double start = 10 * DVD_TIME_BASE;
  Decode(start);
  Decode(start + FRAME);

  // half a second back is a stale picture, not a neighbour's label
  ASSERT_EQ(CDVDVideoCodec::VC_PICTURE, Decode(start + FRAME - DVD_TIME_BASE / 2));
  EXPECT_EQ(1, Out().id);
  EXPECT_EQ(start + FRAME, pic.pts) << "the good picture took the stale label";
  EXPECT_FALSE(Dropped());

  ASSERT_EQ(CDVDVideoCodec::VC_PICTURE, Decode(start + 2 * FRAME));
  EXPECT_EQ(2, Out().id);
  EXPECT_EQ(start + FRAME - DVD_TIME_BASE / 2, pic.pts);
  EXPECT_TRUE(Dropped()) << "shown, it would step the film back";
}

TEST_F(MvcReorder, TheSeekDropGoesWithItsLabel)
{
  Decode(0);
  Decode(2 * FRAME, std::nullopt, DVP_FLAG_DROPPED);

  ASSERT_EQ(CDVDVideoCodec::VC_PICTURE, Decode(FRAME));
  EXPECT_EQ(1, Out().id);
  EXPECT_EQ(FRAME, pic.pts);
  EXPECT_FALSE(Dropped()) << "the drop stayed on the picture, not the label";

  ASSERT_EQ(CDVDVideoCodec::VC_PICTURE, Decode(3 * FRAME));
  EXPECT_EQ(2, Out().id);
  EXPECT_EQ(2 * FRAME, pic.pts);
  EXPECT_TRUE(Dropped());
}

TEST_F(MvcReorder, DrainingHandsOutTheHeldPicture)
{
  Decode(0);
  Decode(FRAME);
  EXPECT_EQ(0, Out().id);

  ASSERT_EQ(CDVDVideoCodec::VC_PICTURE, Drain(CDVDVideoCodec::VC_EOF));
  EXPECT_EQ(1, Out().id);
  EXPECT_EQ(FRAME, pic.pts);
  EXPECT_EQ(nullptr, codec.m_held.videoBuffer);

  EXPECT_EQ(CDVDVideoCodec::VC_EOF, Drain(CDVDVideoCodec::VC_EOF));
}

TEST_F(MvcReorder, DrainingHandsAPictureStraightOutWhenNoneIsHeld)
{
  codec.m_codecControlFlags = DVD_CODEC_CTRL_DRAIN;
  ASSERT_EQ(CDVDVideoCodec::VC_PICTURE, Decode(0));
  EXPECT_EQ(0, Out().id);
  EXPECT_EQ(nullptr, codec.m_held.videoBuffer);
}

TEST_F(MvcReorder, AnErrorWhileDrainingKeepsTheHeldPicture)
{
  Decode(0);
  EXPECT_EQ(CDVDVideoCodec::VC_ERROR, Drain(CDVDVideoCodec::VC_ERROR));
  EXPECT_NE(nullptr, codec.m_held.videoBuffer);
}

TEST_F(MvcReorder, ResettingTheHeldPictureReturnsItsBuffer)
{
  Decode(0);
  Decode(2 * FRAME);
  Decode(FRAME);
  codec.m_held.Reset();
  CheckOwners();
  EXPECT_EQ(0, buffers[2].refs);
}
} // namespace
