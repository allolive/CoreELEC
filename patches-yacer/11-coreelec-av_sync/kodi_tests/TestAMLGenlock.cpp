/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  The display genlock: it steps the master clock once, where playback is
 *  already discontinuous, so that frames land on a vblank instead of halfway
 *  between two. Every collaborator it reads - the clock, the process info and
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
// back what the genlock asked the clock to do.
struct World
{
  double clock = 0.0;
  double absolute = 0.0;
  double vsyncAdjust = 0.0;
  bool renderClockSync = false;
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
bool CProcessInfo::IsRenderClockSync()
{
  return g_world.renderClockSync;
}
unsigned int CProcessInfo::GetMaxPassthroughOffSyncDuration() const
{
  return g_world.maxOffSyncMs;
}
CVideoSettings CProcessInfo::GetVideoSettings()
{
  return {};
}

double aml_refreshes_per_frame(double, double)
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
// Drive one frame. The phase the genlock sees is
// clock - sinceFrameStart + latency - framePts, folded into one period.
void Frame(CAMLGenlock& genlock, double displayFps, double phaseUs)
{
  CDVDClock clock;
  TestProcessInfo processInfo;
  g_world.clock = phaseUs;
  genlock.Update(displayFps, clock, processInfo, StreamHdrType::HDR_TYPE_NONE, 0.0);
}
} // namespace

TEST(AMLGenlock, AlignsTheClockWhenAnAlignmentIsOwed)
{
  Reset();
  g_world.applyResult = -2000.0;
  CAMLGenlock genlock;
  genlock.Restart();
  Frame(genlock, 24.0, 2000.0);

  ASSERT_EQ(g_world.corrections, 1);
  // It asks for the phase to be removed, not added.
  EXPECT_DOUBLE_EQ(g_world.lastCorrection, -2000.0);
}

TEST(AMLGenlock, LeavesARunningPictureAlone)
{
  Reset();
  g_world.applyResult = -2000.0;
  CAMLGenlock genlock;
  genlock.Restart();
  Frame(genlock, 24.0, 2000.0);
  ASSERT_EQ(g_world.corrections, 1);

  // Nothing asked for another alignment, and the display and the clock do not
  // walk apart while playing. Stepping the clock under a smooth picture is the
  // one thing this must never do.
  Frame(genlock, 24.0, 2000.0);
  EXPECT_EQ(g_world.corrections, 1);
}

TEST(AMLGenlock, IgnoresPhaseInsideTheNoiseFloor)
{
  Reset();
  CAMLGenlock genlock;
  genlock.Restart();
  // Under half a millisecond is a measurement, not a misalignment.
  Frame(genlock, 24.0, 200.0);
  EXPECT_EQ(g_world.corrections, 0);
}

TEST(AMLGenlock, StandsDownUntilThePipelineHasBeenMeasured)
{
  Reset();
  CAMLLatencyStore::GetInstance().Set(-1.0f);
  CAMLGenlock genlock;
  genlock.Restart();
  Frame(genlock, 24.0, 2000.0);
  // Aiming with the buffer-count estimate would step the clock by its error.
  EXPECT_EQ(g_world.corrections, 0);
  CAMLLatencyStore::GetInstance().Set(50.0f);
}

TEST(AMLGenlock, StandsDownWhileTheRendererIsSyncingTheClock)
{
  Reset();
  g_world.renderClockSync = true;
  CAMLGenlock genlock;
  genlock.Restart();
  Frame(genlock, 24.0, 2000.0);
  EXPECT_EQ(g_world.corrections, 0);
}

TEST(AMLGenlock, LeavesPulldownAlone)
{
  Reset();
  // 2:3 pulldown gives no whole number of refreshes per frame, so there is no
  // fixed phase to align to.
  g_world.refreshesPerFrame = 2.5;
  CAMLGenlock genlock;
  genlock.Restart();
  Frame(genlock, 60.0, 2000.0);
  EXPECT_EQ(g_world.corrections, 0);
}

TEST(AMLGenlock, WaitsWhileTheVsyncAdjustIsRunning)
{
  Reset();
  g_world.vsyncAdjust = 1.0;
  CAMLGenlock genlock;
  genlock.Restart();
  Frame(genlock, 24.0, 2000.0);
  // ErrorAdjust quantises a step this small to a frame or to nothing while the
  // adjust is running, so the alignment is left owed instead.
  EXPECT_EQ(g_world.corrections, 0);
}

TEST(AMLGenlock, KeepsTheAlignmentOwedWhenTheClockDeclines)
{
  Reset();
  g_world.applyResult = 0.0; // the clock refused
  CAMLGenlock genlock;
  genlock.Restart();
  Frame(genlock, 24.0, 2000.0);
  ASSERT_EQ(g_world.corrections, 1);

  // Still owed, so the next frame tries again rather than giving up.
  Frame(genlock, 24.0, 2000.0);
  EXPECT_EQ(g_world.corrections, 2);
}

TEST(AMLGenlock, AsksAgainWhenTheDisplayRateChanges)
{
  Reset();
  g_world.applyResult = -2000.0;
  CAMLGenlock genlock;
  genlock.Restart();
  Frame(genlock, 24.0, 2000.0);
  ASSERT_EQ(g_world.corrections, 1);

  // A mode change voids the earlier alignment without anybody asking.
  Frame(genlock, 50.0, 2000.0);
  EXPECT_EQ(g_world.corrections, 2);
}

// The dead band exists so that a correction cannot itself push the audio loop
// past its own threshold and start a standing oscillation. The phase is taken
// modulo the period, so it can never exceed half a period - which means the
// guard can only ever be reached where half a period is itself larger than the
// smallest dead band the audio loop allows. At 24p it is; at 25Hz and above it
// is not, and there no correction can be large enough to need it.
TEST(AMLGenlock, RefusesACorrectionAsLargeAsTheAudioDeadBand)
{
  Reset();
  g_world.maxOffSyncMs = 20;
  g_world.applyResult = -1.0;
  CAMLGenlock genlock;
  genlock.Restart();
  // Half a period at 24p is 20.83ms, so a phase beyond the 20ms dead band is
  // reachable here and nowhere above it.
  Frame(genlock, 24.0, DVD_MSEC_TO_TIME(20.5));
  EXPECT_EQ(g_world.corrections, 0);
}

TEST(AMLGenlock, CorrectsJustInsideTheAudioDeadBand)
{
  Reset();
  g_world.maxOffSyncMs = 20;
  g_world.applyResult = -1.0;
  CAMLGenlock genlock;
  genlock.Restart();
  Frame(genlock, 24.0, DVD_MSEC_TO_TIME(19.5));
  EXPECT_EQ(g_world.corrections, 1);
}

// Records the arithmetic rather than the code: the phase is bounded by half a
// period, so at every refresh rate at or above 25Hz it is smaller than the
// smallest dead band and the guard above is unreachable by construction.
TEST(AMLGenlock, PhaseCannotReachTheDeadBandAtOrdinaryRefreshRates)
{
  for (double rate : {60.0, 59.94, 50.0, 30.0})
  {
    const double halfPeriodMs = Period(rate) / 2.0 / 1000.0;
    EXPECT_LT(halfPeriodMs, 20.0) << rate << "Hz";
  }
  // At 24p it is reachable, which is why the guard is there at all.
  EXPECT_GT(Period(24.0) / 2.0 / 1000.0, 20.0);
}
