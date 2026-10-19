/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  The display genlock: once, where playback is already discontinuous, it
 *  places the picture so that frames land on a vblank instead of halfway
 *  between two, and leaves the master clock alone. Every collaborator it reads - the clock, the process info and
 *  the three AML helpers - is an ordinary non-virtual function, so the test
 *  binary defines them. That is a link seam, not a mock: no mocking framework
 *  can hook a non-virtual call, and none is needed.
 */

#include "cores/VideoPlayer/DVDCodecs/Video/AMLGenlock.h"

#include "cores/VideoPlayer/DVDCodecs/Video/AMLLatency.h"
#include "cores/VideoPlayer/DVDClock.h"
// CDVDClock holds a unique_ptr to this, so destroying one needs it complete.
#include "cores/VideoPlayer/VideoReferenceClock.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "cores/VideoPlayer/Process/ProcessInfo.h"
#include "platform/posix/threads/RecursiveMutex.h"
#include "utils/AMLUtils.h"

#include <cmath>
#include <optional>
#include <string>

#include <gtest/gtest.h>

namespace
{
// What the seams below answer. A test sets these, calls Update, and reads
// back the placement and anything the genlock asked the clock to do.
struct World
{
  double clock = 0.0;
  double absolute = 0.0;
  double vsyncAdjust = 0.0;
  float videoFps = 24.0f;
  unsigned int maxOffSyncMs = 50;
  std::optional<double> sinceFrameStart = 0.0;
  double latency = 0.0;
  double refreshesPerFrame = 1.0;

  int corrections = 0;
  double lastCorrection = 0.0;
  double applyResult = 0.0; // what ErrorAdjust answers; 0 means it declined
};

World g_world;

void Reset(double displayFps = 24.0)
{
  g_world = World{};
  g_world.videoFps = static_cast<float>(displayFps);
  CAMLGenlock::ResetPlacement();
  // A measured pipeline, so the genlock has something to aim with.
  CAMLLatencyStore::GetInstance().Set(50.0f);
}

double Period(double fps)
{
  return DVD_TIME_BASE / fps;
}

// Something CProcessInfo's protected constructor allows.
class TestProcessInfo : public CProcessInfo
{
};
} // namespace

// The seams. Each ignores its object and answers the world above.
CDVDClock::CDVDClock() = default;
CDVDClock::~CDVDClock() = default;
double CDVDClock::GetClock(bool)
{
  return g_world.clock;
}
double CDVDClock::GetAbsoluteClock(bool)
{
  return g_world.absolute;
}
double CDVDClock::GetVsyncAdjust()
{
  return g_world.vsyncAdjust;
}
double CDVDClock::ErrorAdjust(double error, const char*)
{
  ++g_world.corrections;
  g_world.lastCorrection = error;
  return g_world.applyResult;
}

CProcessInfo::CProcessInfo() = default;
// The rest of CProcessInfo's own interface, so that one can be constructed.
// None of it is under test; the genlock never calls any of it.
EINTERLACEMETHOD CProcessInfo::GetFallbackDeintMethod()
{
  return EINTERLACEMETHOD::VS_INTERLACEMETHOD_NONE;
}
void CProcessInfo::SetSwDeinterlacingMethods()
{
}
bool CProcessInfo::AllowDTSHDDecode()
{
  return true;
}
std::vector<AVPixelFormat> CProcessInfo::GetRenderFormats()
{
  return {};
}
float CProcessInfo::MinTempoPlatform()
{
  return 0.75f;
}
float CProcessInfo::MaxTempoPlatform()
{
  return 1.5f;
}

CVideoSettings::CVideoSettings() = default;
CVideoBufferManager::CVideoBufferManager() = default;

pthread_mutexattr_t& XbmcThreads::CRecursiveMutex::getRecursiveAttr()
{
  static pthread_mutexattr_t attr = []
  {
    pthread_mutexattr_t created;
    pthread_mutexattr_init(&created);
    pthread_mutexattr_settype(&created, PTHREAD_MUTEX_RECURSIVE);
    return created;
  }();
  return attr;
}
float CProcessInfo::GetVideoFps()
{
  return g_world.videoFps;
}
unsigned int CProcessInfo::GetMaxPassthroughOffSyncDuration() const
{
  return g_world.maxOffSyncMs;
}
CVideoSettings CProcessInfo::GetVideoSettings()
{
  return {};
}

double aml_refreshes_per_frame()
{
  return g_world.refreshesPerFrame;
}
std::optional<double> aml_since_frame_start_us(double)
{
  return g_world.sinceFrameStart;
}
double aml_render_display_latency(StreamHdrType, float)
{
  return g_world.latency;
}

namespace
{
// Present one frame with the clock as it stands.
void Present(CAMLGenlock& genlock, double displayFps)
{
  CDVDClock clock;
  TestProcessInfo processInfo;
  genlock.Update(displayFps, clock, processInfo, StreamHdrType::HDR_TYPE_NONE, 0.0);
}

// Drive one frame. The phase the genlock sees is
// clock - sinceFrameStart + latency - framePts, folded into one period.
// Setting the clock without its reference is a step when it moves it 10ms.
void Frame(CAMLGenlock& genlock, double displayFps, double phaseUs)
{
  g_world.clock = phaseUs;
  Present(genlock, displayFps);
}

float Ms(double us)
{
  return static_cast<float>(us / 1000.0);
}
} // namespace

TEST(AMLGenlock, PlacesThePictureWhenAnAlignmentIsOwed)
{
  Reset();
  CAMLGenlock genlock;
  genlock.Restart();
  Frame(genlock, 24.0, 2000.0);

  // The phase itself: the windowing system adds it to the frame latency the
  // renderer subtracts, which moves the picture as stepping the clock back by
  // it would have.
  EXPECT_FLOAT_EQ(CAMLGenlock::Placement(), 2.0f);
  EXPECT_EQ(g_world.corrections, 0) << "the clock the audio is on is never stepped";
  EXPECT_DOUBLE_EQ(g_world.clock, 2000.0);
}

TEST(AMLGenlock, LeavesARunningPictureAlone)
{
  Reset();
  CAMLGenlock genlock;
  genlock.Restart();
  Frame(genlock, 24.0, 2000.0);
  ASSERT_FLOAT_EQ(CAMLGenlock::Placement(), 2.0f);

  // Nothing asked for another alignment, and the display and the clock do not
  // walk apart while playing: a different reading now is not acted on.
  g_world.sinceFrameStart = 5000.0;
  Present(genlock, 24.0);
  EXPECT_FLOAT_EQ(CAMLGenlock::Placement(), 2.0f);
  EXPECT_EQ(g_world.corrections, 0);
}

TEST(AMLGenlock, IgnoresPhaseInsideTheNoiseFloor)
{
  Reset();
  CAMLGenlock genlock;
  genlock.Restart();
  // Under half a millisecond is a measurement, not a misalignment.
  Frame(genlock, 24.0, 200.0);
  EXPECT_FLOAT_EQ(CAMLGenlock::Placement(), 0.0f);

  // The same against a placement already made: it is not moved by noise.
  genlock.Restart();
  Frame(genlock, 24.0, 2000.0);
  ASSERT_FLOAT_EQ(CAMLGenlock::Placement(), 2.0f);
  genlock.Restart();
  Frame(genlock, 24.0, 2400.0);
  EXPECT_FLOAT_EQ(CAMLGenlock::Placement(), 2.0f);
  genlock.Restart();
  Frame(genlock, 24.0, 2600.0);
  EXPECT_FLOAT_EQ(CAMLGenlock::Placement(), 2.6f);
}

TEST(AMLGenlock, StandsDownUntilThePipelineHasBeenMeasured)
{
  Reset();
  CAMLLatencyStore::GetInstance().Set(-1.0f);
  CAMLGenlock genlock;
  genlock.Restart();
  Frame(genlock, 24.0, 2000.0);
  // Aiming with the buffer-count estimate would place the picture by its error.
  EXPECT_FLOAT_EQ(CAMLGenlock::Placement(), 0.0f);

  // Still owed once the measurement is in.
  CAMLLatencyStore::GetInstance().Set(50.0f);
  Frame(genlock, 24.0, 2000.0);
  EXPECT_FLOAT_EQ(CAMLGenlock::Placement(), 2.0f);
}

TEST(AMLGenlock, LeavesPulldownAlone)
{
  Reset();
  // 2:3 pulldown gives no whole number of refreshes per frame, so there is no
  // fixed phase to align to. CRenderManager finds no match, and the ratio
  // reads zero.
  g_world.refreshesPerFrame = 0.0;
  CAMLGenlock genlock;
  genlock.Restart();
  Frame(genlock, 60.0, 2000.0);
  EXPECT_FLOAT_EQ(CAMLGenlock::Placement(), 0.0f);
  EXPECT_EQ(g_world.corrections, 0);
}

// Just past the early branch's threshold, where a frame lands close to on time
// and clear of the fold at the threshold itself.
TEST(AMLGenlock, AimsJustPastTheEarlyThresholdWhenFramesRepeat)
{
  Reset(48.0);
  g_world.refreshesPerFrame = 2.0;
  CAMLGenlock genlock;
  genlock.Restart();
  Frame(genlock, 48.0, 6000.0);
  EXPECT_FLOAT_EQ(CAMLGenlock::Placement(), Ms(6000.0 - 1500.0));
}

TEST(AMLGenlock, PlacesThePictureWhileTheVsyncAdjustIsRunning)
{
  Reset();
  g_world.vsyncAdjust = 1.0;
  CAMLGenlock genlock;
  genlock.Restart();
  Frame(genlock, 24.0, 2000.0);
  // No clock step for the adjust to quantise, so nothing to wait for.
  EXPECT_FLOAT_EQ(CAMLGenlock::Placement(), 2.0f);
  EXPECT_EQ(g_world.corrections, 0);
}

TEST(AMLGenlock, KeepsTheAlignmentOwedWithoutAVblankReading)
{
  Reset();
  g_world.sinceFrameStart = std::nullopt;
  CAMLGenlock genlock;
  genlock.Restart();
  Frame(genlock, 24.0, 2000.0);
  EXPECT_FLOAT_EQ(CAMLGenlock::Placement(), 0.0f);

  // Still owed, so the next frame with a reading places the picture.
  g_world.sinceFrameStart = 0.0;
  Frame(genlock, 24.0, 2000.0);
  EXPECT_FLOAT_EQ(CAMLGenlock::Placement(), 2.0f);
}

TEST(AMLGenlock, AsksAgainWhenTheDisplayRateChanges)
{
  Reset();
  CAMLGenlock genlock;
  genlock.Restart();
  Frame(genlock, 24.0, 2000.0);
  ASSERT_FLOAT_EQ(CAMLGenlock::Placement(), 2.0f);

  // A mode change voids the earlier placement at once, before a reading.
  g_world.sinceFrameStart = std::nullopt;
  Frame(genlock, 50.0, 2000.0);
  EXPECT_FLOAT_EQ(CAMLGenlock::Placement(), 0.0f);

  // And realigns without anybody asking.
  g_world.sinceFrameStart = 0.0;
  Frame(genlock, 50.0, 3000.0);
  EXPECT_FLOAT_EQ(CAMLGenlock::Placement(), 3.0f);
}

TEST(AMLGenlock, AsksAgainWhenSomebodyStepsTheClock)
{
  Reset();
  CAMLGenlock genlock;
  genlock.Restart();
  Frame(genlock, 24.0, 2000.0);
  ASSERT_FLOAT_EQ(CAMLGenlock::Placement(), 2.0f);

  // Under the step threshold is arithmetic, not a step.
  Frame(genlock, 24.0, 7000.0);
  EXPECT_FLOAT_EQ(CAMLGenlock::Placement(), 2.0f);

  // The audio loop moved the clock 30ms: the frames moved with it.
  Frame(genlock, 24.0, 37000.0);
  EXPECT_FLOAT_EQ(CAMLGenlock::Placement(), Ms(37000.0 - Period(24.0)));
  EXPECT_EQ(g_world.corrections, 0);
}

TEST(AMLGenlock, TakesThePlacementNearestTheCurrentOneOnTheFold)
{
  Reset();
  CAMLGenlock genlock;
  genlock.Restart();
  Frame(genlock, 24.0, -DVD_MSEC_TO_TIME(20.5));
  ASSERT_FLOAT_EQ(CAMLGenlock::Placement(), -20.5f);

  // Just past half a period the other way is the same phase a period away;
  // taking it as read would move the picture a whole frame.
  genlock.Restart();
  Frame(genlock, 24.0, DVD_MSEC_TO_TIME(20.6));
  EXPECT_FLOAT_EQ(CAMLGenlock::Placement(), Ms(DVD_MSEC_TO_TIME(20.6) - Period(24.0)));
}

// The audio loop's dead band bounded the old clock step, so that the step could
// not push the audio error past its own threshold and start an oscillation. A
// placement moves no clock, so it is not bounded by the dead band at all.
TEST(AMLGenlock, PlacesBeyondTheAudioDeadBand)
{
  Reset();
  g_world.maxOffSyncMs = 20;
  CAMLGenlock genlock;
  genlock.Restart();
  // Half a period at 24p is 20.83ms, so a phase beyond the 20ms dead band is
  // reachable here.
  Frame(genlock, 24.0, DVD_MSEC_TO_TIME(20.5));
  EXPECT_FLOAT_EQ(CAMLGenlock::Placement(), 20.5f);
  EXPECT_EQ(g_world.corrections, 0);
}

TEST(AMLGenlock, PlacesJustInsideTheAudioDeadBand)
{
  Reset();
  g_world.maxOffSyncMs = 20;
  CAMLGenlock genlock;
  genlock.Restart();
  Frame(genlock, 24.0, DVD_MSEC_TO_TIME(19.5));
  EXPECT_FLOAT_EQ(CAMLGenlock::Placement(), 19.5f);
  EXPECT_EQ(g_world.corrections, 0);
}

// What the placement shifts the picture against the audio is the audio rate
// trim's to walk out, and it is never more than half a refresh period.
TEST(AMLGenlock, PlacementNeverExceedsHalfAPeriod)
{
  for (double rate : {60.0, 59.94, 50.0, 30.0, 24.0, 23.976})
  {
    Reset(rate);
    CAMLGenlock genlock;
    genlock.Restart();
    Frame(genlock, rate, 0.6 * Period(rate));
    EXPECT_NEAR(CAMLGenlock::Placement(), Ms(-0.4 * Period(rate)), 1e-3) << rate << "Hz";
    EXPECT_LE(std::abs(CAMLGenlock::Placement()), Ms(Period(rate) / 2.0)) << rate << "Hz";
  }
}

TEST(AMLGenlock, PhaseIsWhatAnAlignmentWouldPlace)
{
  Reset();
  g_world.latency = 7000.0;
  g_world.sinceFrameStart = 1000.0;
  TestProcessInfo processInfo;
  const auto phase =
      CAMLGenlock::Phase(24.0, 2000.0, processInfo, StreamHdrType::HDR_TYPE_NONE, 3000.0);
  ASSERT_TRUE(phase);
  EXPECT_DOUBLE_EQ(*phase, 5000.0);

  CAMLGenlock genlock;
  genlock.Restart();
  g_world.clock = 2000.0;
  CDVDClock clock;
  genlock.Update(24.0, clock, processInfo, StreamHdrType::HDR_TYPE_NONE, 3000.0);
  EXPECT_FLOAT_EQ(CAMLGenlock::Placement(), Ms(*phase));

  g_world.sinceFrameStart = std::nullopt;
  EXPECT_FALSE(CAMLGenlock::Phase(24.0, 2000.0, processInfo, StreamHdrType::HDR_TYPE_NONE, 0.0));
  g_world.sinceFrameStart = 0.0;
  g_world.refreshesPerFrame = 0.0;
  EXPECT_FALSE(CAMLGenlock::Phase(24.0, 2000.0, processInfo, StreamHdrType::HDR_TYPE_NONE, 0.0));
  g_world.refreshesPerFrame = 1.0;
  EXPECT_FALSE(CAMLGenlock::Phase(0.0, 2000.0, processInfo, StreamHdrType::HDR_TYPE_NONE, 0.0));
}

// CRenderManager hands over its refreshes per frame; only a whole number of
// them is something to align to.
TEST(AMLGenlock, WholeRefreshesPerFrame)
{
  EXPECT_EQ(aml_whole_refreshes(0.0), 0.0); // no match
  EXPECT_EQ(aml_whole_refreshes(1.0), 1.0);
  EXPECT_EQ(aml_whole_refreshes(2.0), 2.0);
  // 23.976 divided in float lands a rounding error either side of the content
  EXPECT_EQ(aml_whole_refreshes(0.99999992), 1.0);
  EXPECT_EQ(aml_whole_refreshes(1.00000008), 1.0);
  EXPECT_EQ(aml_whole_refreshes(1.9999999), 2.0);
  // Content faster than the display, which Kodi counts as a match: frames are
  // dropped, not each shown a whole number of times
  EXPECT_EQ(aml_whole_refreshes(0.5), 0.0);        // 48fps on 24Hz
  EXPECT_EQ(aml_whole_refreshes(0.49999997), 0.0); // the same in float
  EXPECT_EQ(aml_whole_refreshes(1.0 / 3.0), 0.0);  // 72fps on 24Hz
  // the edge of CheckEnableClockSync()'s own tolerance below one
  EXPECT_EQ(aml_whole_refreshes(1.0 / 1.0004999), 1.0);
  EXPECT_EQ(aml_whole_refreshes(0.9994), 0.0);
}
