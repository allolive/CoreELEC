#include "cores/VideoPlayer/PlaybackPreroll.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>

namespace
{
using Preroll = CPlaybackPreroll;
using Action = Preroll::Action;
constexpr double NOW = 10000000.0;
constexpr double TARGET = 1000000.0;
constexpr uint64_t GENERATION = 7;

// Fatal, so a failed precondition stops the case where it happened rather
// than reporting every consequence of it. Helpers that use it are called
// through ASSERT_NO_FATAL_FAILURE, which is what makes that stop propagate.
#define Require(condition, message) ASSERT_TRUE(condition) << (message)

Preroll::Audio Audio(double now = NOW, bool synced = false, bool held = false)
{
  return {500000.0, 800000.0, 550000.0, now, synced, held};
}

void Start(Preroll& preroll, double target = TARGET, double lead = 100000.0)
{
  Require(preroll.Begin(GENERATION, target, NOW), "begin failed");
  Require(preroll.SetVideoReady(GENERATION, target, NOW, lead), "video readiness rejected");
  Require(preroll.SetAudio(GENERATION, Audio(), NOW), "initial audio rejected");
  auto decision = preroll.Poll(NOW);
  Require(decision.action == Action::StartAudio && decision.clockPts == 550000.0 &&
              decision.targetPts == target && preroll.Active(),
          "start did not use accepted packet end minus delay");
}

// Preparation that runs behind a picture the viewer already froze. It parks
// instead of starting audio, so the pause is not charged against the
// preparation deadline.
void Arm(Preroll& preroll, double target = TARGET, double lead = 100000.0)
{
  Require(preroll.Begin(GENERATION, target, NOW), "begin failed");
  preroll.ArmWhenPrepared();
  Require(preroll.SetVideoReady(GENERATION, target, NOW, lead), "video readiness rejected");
  Require(preroll.SetAudio(GENERATION, Audio(), NOW), "initial audio rejected");
  const auto decision = preroll.Poll(NOW);
  Require(decision.action == Action::None, "arming started audio behind the pause");
  Require(preroll.Armed(), "prepared work did not park in the armed stage");
}

TEST(PlaybackPreroll, IndependentPreparation)
{
  Preroll preroll;
  Require(!preroll.Active() && preroll.Poll(NOW).action == Action::None, "idle permission");
  Require(preroll.Begin(GENERATION, TARGET, NOW), "valid begin rejected");
  Require(preroll.SetAudio(GENERATION, Audio(), NOW), "audio-first report rejected");
  Require(preroll.Poll(NOW).action == Action::None, "audio released without target picture");
  Require(preroll.SetVideoReady(GENERATION, TARGET, NOW), "video readiness rejected");
  Require(preroll.Poll(NOW).action == Action::StartAudio, "ready endpoints did not start audio");
  Require(preroll.Poll(NOW).action == Action::None, "audio started twice");

  preroll.Cancel();
  Require(preroll.Begin(8, TARGET, NOW), "replacement begin rejected");
  Require(preroll.SetVideoReady(8, TARGET, NOW), "video-first report rejected");
  Require(preroll.Poll(NOW).action == Action::None, "video released without buffered audio");
  Require(preroll.SetAudio(8, Audio(), NOW), "replacement audio rejected");
  Require(preroll.Poll(NOW).action == Action::StartAudio, "video-first preparation failed");
}

TEST(PlaybackPreroll, FreshRunningEvidenceAndOneShotRelease)
{
  Preroll preroll;
  Require(preroll.Begin(GENERATION, TARGET, NOW), "begin rejected");
  Require(preroll.SetVideoReady(GENERATION, TARGET, NOW), "video rejected");
  Require(preroll.SetAudio(GENERATION, Audio(NOW, true), NOW), "paused INSYNC rejected as data");
  Require(preroll.Poll(NOW).action == Action::StartAudio, "audio did not start");
  Require(preroll.Poll(NOW + 1).action == Action::None, "paused INSYNC survived start invalidation");
  Require(preroll.SetAudio(GENERATION, Audio(NOW, true), NOW + 1), "equal-time report rejected");
  Require(preroll.Poll(NOW + 1).action == Action::None, "pre-start sample certified running audio");
  Require(preroll.SetAudio(GENERATION, Audio(NOW + 2, true), NOW + 2), "fresh report rejected");
  const auto decision = preroll.Poll(NOW + 2);
  Require(decision.action == Action::Release && decision.generation == GENERATION &&
              decision.targetPts == TARGET && preroll.Active() && preroll.Releasing(),
          "fresh INSYNC with runway failed to release");
  Require(preroll.Poll(NOW + 3).action == Action::None,
          "pending audio handoff sent duplicate release");
  Require(!preroll.SetAudio(GENERATION - 1, Audio(NOW + 3, true), NOW + 3) &&
              preroll.Releasing() && preroll.Generation() == GENERATION,
          "stale acknowledgement changed pending handoff identity");
  preroll.Cancel(); // Caller has verified the matching audio handoff and video lead.
  Require(!preroll.Active() && !preroll.Releasing() &&
              !preroll.SetAudio(GENERATION, Audio(NOW + 4, true), NOW + 4),
          "completed generation retained permission");
}

TEST(PlaybackPreroll, NoFalseEarlyQueueExhaustion)
{
  Preroll preroll;
  ASSERT_NO_FATAL_FAILURE(Start(preroll));
  auto audio = Audio(NOW + 1);
  audio.playingPts = 790000.0;
  Require(preroll.SetAudio(GENERATION, audio, NOW + 1), "short queue report rejected");
  Require(preroll.Poll(NOW + 1).action == Action::None,
          "filling queue mistaken for exhausted intentional preroll");
  audio.hostUs = NOW + 2;
  audio.mainHeld = true;
  Require(preroll.SetAudio(GENERATION, audio, NOW + 2), "boundary report rejected");
  Require(preroll.Poll(NOW + 2).action == Action::Fail,
          "exhausted preroll silently authorized target audio");
}

TEST(PlaybackPreroll, ExactRunwayBoundary)
{
  Preroll preroll;
  ASSERT_NO_FATAL_FAILURE(Start(preroll));
  auto audio = Audio(NOW + 1, true, true);
  audio.submittedEndPts = TARGET;
  audio.playingPts = TARGET - 100000.0;
  Require(preroll.SetAudio(GENERATION, audio, NOW + 1), "exact boundary report rejected");
  Require(preroll.Poll(NOW + 1).action == Action::Release, "exact100ms margins rejected");

  ASSERT_NO_FATAL_FAILURE(Start(preroll));
  audio.hostUs = NOW + 1;
  audio.playingPts += 0.25;
  Require(preroll.SetAudio(GENERATION, audio, NOW + 1), "fractional report rejected");
  Require(preroll.Poll(NOW + 1).action == Action::Fail,
          "fractional shortfall allowed unsafe release even though INSYNC");
}

TEST(PlaybackPreroll, SnapshotAgeConsumesMargin)
{
  Preroll preroll;
  ASSERT_NO_FATAL_FAILURE(Start(preroll));
  auto audio = Audio(NOW + 1, true, true);
  audio.submittedEndPts = TARGET;
  audio.playingPts = TARGET - 100000.0;
  Require(preroll.SetAudio(GENERATION, audio, NOW + 99001), "freshness-bounded report rejected");
  Require(preroll.Poll(NOW + 99001).action == Action::Fail,
          "99ms-old report spent the same100ms runway twice");
}

TEST(PlaybackPreroll, PresentationLeadAndRetarget)
{
  Preroll preroll;
  ASSERT_NO_FATAL_FAILURE(Start(preroll, TARGET, 250000.0));
  auto audio = Audio(NOW + 1, true, true);
  audio.submittedEndPts = TARGET;
  audio.playingPts = TARGET - 150000.0;
  Require(preroll.SetAudio(GENERATION, audio, NOW + 1), "lead sample rejected");
  Require(preroll.Poll(NOW + 1).action == Action::Fail,
          "raw100ms margin ignored larger renderer presentation lead");

  Require(preroll.Begin(GENERATION, TARGET, NOW), "retarget begin rejected");
  Require(!preroll.SetVideoReady(GENERATION, TARGET - 1.0, NOW), "earlier target accepted");
  Require(!preroll.SetVideoReady(GENERATION, TARGET + 100001.0, NOW), "large target jump accepted");
  Require(preroll.SetVideoReady(GENERATION, TARGET + 41708.375, NOW), "next picture rejected");
  Require(preroll.TargetPts() == TARGET + 41708.375, "target lost fractional timestamp");
  Require(!preroll.SetVideoReady(GENERATION, TARGET + 50000.0, NOW), "held picture changed identity");
  Require(preroll.SetVideoReady(GENERATION, TARGET + 41708.375, NOW, 275000.0),
          "same picture could not update latency requirement");
  Require(preroll.SetAudio(GENERATION, Audio(), NOW), "initial retarget audio rejected");
  Require(preroll.Poll(NOW).targetPts == TARGET + 41708.375,
          "start failed to publish final picture target");
}

TEST(PlaybackPreroll, GenerationsAndReorderedReports)
{
  Preroll preroll;
  ASSERT_NO_FATAL_FAILURE(Start(preroll));
  Require(!preroll.SetAudio(GENERATION - 1, Audio(NOW + 1, true), NOW + 1), "old audio accepted");
  Require(!preroll.SetVideoReady(GENERATION + 1, TARGET, NOW + 1), "future video accepted");
  auto audio = Audio(NOW + 10);
  Require(preroll.SetAudio(GENERATION, audio, NOW + 10), "fresh report rejected");
  audio.hostUs = NOW + 5;
  Require(!preroll.SetAudio(GENERATION, audio, NOW + 10), "out-of-order audio accepted");
  audio.hostUs = NOW + 11;
  audio.submittedEndPts -= 1;
  Require(!preroll.SetAudio(GENERATION, audio, NOW + 11), "submitted timeline regressed");
  audio = Audio(NOW + 11);
  audio.firstPts += 1;
  Require(!preroll.SetAudio(GENERATION, audio, NOW + 11), "first audio identity changed");
  preroll.Cancel();
  Require(!preroll.Active() && preroll.Generation() == 0 &&
              preroll.Poll(NOW + 12).action == Action::None,
          "cancel retained release permission");
  Require(preroll.Begin(GENERATION + 1, TARGET, NOW + 20), "replacement failed");
  Require(!preroll.SetAudio(GENERATION, Audio(NOW + 21, true), NOW + 21),
          "cancelled generation certified replacement");
}

TEST(PlaybackPreroll, InvalidAndStaleInputs)
{
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double infinity = std::numeric_limits<double>::infinity();
  Preroll preroll;
  Require(!preroll.Begin(0, TARGET, NOW) && !preroll.Begin(GENERATION, nan, NOW) &&
              !preroll.Begin(GENERATION, -1.0, NOW) &&
              !preroll.Begin(GENERATION, TARGET, infinity),
          "invalid begin accepted");
  Require(preroll.Begin(GENERATION, TARGET, NOW), "valid begin rejected");
  Require(!preroll.SetVideoReady(GENERATION, nan, NOW) &&
              !preroll.SetVideoReady(GENERATION, TARGET, NOW, infinity) &&
              !preroll.SetVideoReady(GENERATION, TARGET, NOW, -1.0),
          "invalid video evidence accepted");
  auto audio = Audio();
  audio.playingPts = nan;
  Require(!preroll.SetAudio(GENERATION, audio, NOW), "unknown audio PTS accepted");
  audio = Audio();
  audio.submittedEndPts = audio.firstPts;
  Require(!preroll.SetAudio(GENERATION, audio, NOW), "zero accepted audio certified buffering");
  audio = Audio(NOW + 1);
  Require(!preroll.SetAudio(GENERATION, audio, NOW), "future audio sample accepted");
  audio = Audio();
  Require(!preroll.SetAudio(GENERATION, audio, NOW + 100001.0), "stale audio sample accepted");
  Require(preroll.SetVideoReady(GENERATION, TARGET, NOW), "video readiness rejected");
  Require(preroll.SetAudio(GENERATION, Audio(), NOW), "current sample rejected");
  Require(preroll.Poll(NOW + 100001.0).action == Action::None,
          "sample became stale but still started audio");
  Require(preroll.Poll(NOW + 100000.0).action == Action::Fail, "backwards host clock ignored");
}

TEST(PlaybackPreroll, NearStartAndProtectedBoundary)
{
  Preroll preroll;
  Require(preroll.Begin(GENERATION, 250000.0, NOW), "shortened near-start interval rejected");
  Require(preroll.SetVideoReady(GENERATION, 250000.0, NOW), "near-start video rejected");
  auto audio = Preroll::Audio{0.0, 200000.0, -10000.0, NOW, false, false};
  Require(preroll.SetAudio(GENERATION, audio, NOW), "valid negative delay-derived PTS rejected");
  Require(preroll.Poll(NOW).clockPts == -10000.0, "near-start clock anchor was clamped");
  audio.hostUs = NOW + 1;
  audio.playingPts = 0.0;
  audio.inSync = true;
  Require(preroll.SetAudio(GENERATION, audio, NOW + 1), "valid zero playing PTS rejected");
  Require(preroll.Poll(NOW + 1).action == Action::Release, "valid zero PTS treated as absent");

  ASSERT_NO_FATAL_FAILURE(Start(preroll));
  audio = Audio(NOW + 1, true, true);
  audio.submittedEndPts = TARGET + 0.125;
  Require(preroll.SetAudio(GENERATION, audio, NOW + 1), "boundary violation input rejected early");
  Require(preroll.Poll(NOW + 1).action == Action::Fail,
          "target-straddling accepted packet certified protected target");

  Require(preroll.Begin(GENERATION, 50000.0, NOW), "tiny near-start interval rejected");
  Require(preroll.SetVideoReady(GENERATION, 50000.0, NOW), "tiny target rejected");
  audio = {0.0, 40000.0, 0.0, NOW, false, false};
  Require(preroll.SetAudio(GENERATION, audio, NOW), "tiny initial packet rejected");
  Require(preroll.Poll(NOW).action == Action::StartAudio, "tiny interval failed preparation");
  audio.hostUs = NOW + 1;
  audio.inSync = true;
  audio.mainHeld = true;
  Require(preroll.SetAudio(GENERATION, audio, NOW + 1), "tiny boundary report rejected");
  Require(preroll.Poll(NOW + 1).action == Action::Fail,
          "insufficient near-start runway falsely released");
}

TEST(PlaybackPreroll, ExplicitTimeoutsAndNoElapsedTimeSuccess)
{
  Preroll preroll;
  Require(preroll.Begin(GENERATION, TARGET, NOW), "begin rejected");
  Require(preroll.Poll(NOW + 4999999.0).action == Action::None, "preparation expired early");
  Require(preroll.Poll(NOW + 5000000.0).action == Action::Fail, "preparation timeout missing");
  ASSERT_NO_FATAL_FAILURE(Start(preroll));
  Require(preroll.Poll(NOW + 500000.0).action == Action::None,
          "500ms elapsed time was mistaken for synchronized endpoints");
  Require(preroll.Poll(NOW + 2999999.0).action == Action::None, "running timeout fired early");
  const auto failure = preroll.Poll(NOW + 3000000.0);
  Require(failure.action == Action::Fail && failure.generation == GENERATION && !preroll.Active(),
          "running timeout lost failure identity");
  Require(preroll.Poll(NOW + 3000001.0).action == Action::None, "timeout repeated");

  ASSERT_NO_FATAL_FAILURE(Start(preroll));
  Require(preroll.SetAudio(GENERATION, Audio(NOW + 1, true), NOW + 1), "running report rejected");
  Require(preroll.Poll(NOW + 1).action == Action::Release && preroll.Releasing(),
          "handoff did not remain active");
  Require(preroll.Poll(NOW + 2999999.0).action == Action::None, "handoff expired early");
  Require(preroll.Poll(NOW + 3000000.0).action == Action::Fail && !preroll.Active(),
          "missing audio acknowledgement escaped bounded fallback");
}
TEST(PlaybackPreroll, ArmingParksWithoutStartingAudio)
{
  Preroll preroll;
  ASSERT_NO_FATAL_FAILURE(Arm(preroll));
  EXPECT_TRUE(preroll.Active());
  EXPECT_TRUE(preroll.Armed());
  // The audio start has not been issued, so nothing has been running.
  EXPECT_DOUBLE_EQ(preroll.StartedUs(), 0.0);
}

TEST(PlaybackPreroll, ArmedSuspendsThePreparationDeadline)
{
  Preroll preroll;
  ASSERT_NO_FATAL_FAILURE(Arm(preroll));
  // A viewer may hold the pause for as long as they like. Nothing is pending
  // while armed, so nothing can expire.
  const auto decision = preroll.Poll(NOW + 3600000000.0);
  EXPECT_EQ(decision.action, Action::None);
  EXPECT_TRUE(preroll.Active());
  EXPECT_TRUE(preroll.Armed());
}

TEST(PlaybackPreroll, ResumeChargesTheDeadlineFromTheResumeNotTheBegin)
{
  Preroll preroll;
  ASSERT_NO_FATAL_FAILURE(Arm(preroll));
  const double pausedFor = 60000000.0;
  const double resumeAt = NOW + pausedFor;
  ASSERT_TRUE(preroll.Resume(resumeAt));
  EXPECT_FALSE(preroll.Armed());

  // Had the pause been charged, this poll would already be past the deadline.
  EXPECT_EQ(preroll.Poll(resumeAt + 4999999.0).action, Action::None);
  EXPECT_TRUE(preroll.Active());

  const auto expired = preroll.Poll(resumeAt + 5000000.0);
  EXPECT_EQ(expired.action, Action::Fail);
  EXPECT_STREQ(expired.reason, "preparation timeout");
  EXPECT_FALSE(preroll.Active());
}

TEST(PlaybackPreroll, ResumeStartsAudioOnceTheSampleIsFreshAgain)
{
  Preroll preroll;
  ASSERT_NO_FATAL_FAILURE(Arm(preroll));
  const double resumeAt = NOW + 60000000.0;
  ASSERT_TRUE(preroll.Resume(resumeAt));

  // The sample taken before the pause is far too old to certify anything.
  EXPECT_EQ(preroll.Poll(resumeAt).action, Action::None);

  ASSERT_TRUE(preroll.SetAudio(GENERATION, Audio(resumeAt), resumeAt));
  const auto decision = preroll.Poll(resumeAt);
  EXPECT_EQ(decision.action, Action::StartAudio);
  EXPECT_DOUBLE_EQ(decision.clockPts, 550000.0);
  EXPECT_DOUBLE_EQ(decision.targetPts, TARGET);
  EXPECT_DOUBLE_EQ(preroll.StartedUs(), resumeAt);
}

TEST(PlaybackPreroll, ResumeRefusedUnlessArmed)
{
  Preroll idle;
  EXPECT_FALSE(idle.Resume(NOW));

  Preroll preparing;
  ASSERT_TRUE(preparing.Begin(GENERATION, TARGET, NOW));
  EXPECT_FALSE(preparing.Resume(NOW));

  Preroll running;
  ASSERT_NO_FATAL_FAILURE(Start(running));
  EXPECT_FALSE(running.Resume(NOW));
}

TEST(PlaybackPreroll, ResumeRefusesAnUnusableHostTime)
{
  Preroll preroll;
  ASSERT_NO_FATAL_FAILURE(Arm(preroll));
  EXPECT_FALSE(preroll.Resume(-1.0));
  EXPECT_FALSE(preroll.Resume(std::numeric_limits<double>::quiet_NaN()));
  EXPECT_TRUE(preroll.Armed());
}

TEST(PlaybackPreroll, CancelClearsTheArming)
{
  Preroll preroll;
  ASSERT_TRUE(preroll.Begin(GENERATION, TARGET, NOW));
  preroll.ArmWhenPrepared();
  preroll.Cancel();
  // A preroll begun afterwards must start audio, not park: the arming
  // belonged to the pause that was cancelled.
  ASSERT_NO_FATAL_FAILURE(Start(preroll));
  EXPECT_FALSE(preroll.Armed());
}

TEST(PlaybackPreroll, ArmingDoesNotMaskAnUnprotectedInterval)
{
  // Audio already submitted past the target leaves no interval to protect.
  // That is decided before the arming is honoured.
  const double target = 700000.0;
  Preroll preroll;
  ASSERT_TRUE(preroll.Begin(GENERATION, target, NOW));
  preroll.ArmWhenPrepared();
  ASSERT_TRUE(preroll.SetVideoReady(GENERATION, target, NOW));
  ASSERT_TRUE(preroll.SetAudio(GENERATION, Audio(), NOW));
  const auto decision = preroll.Poll(NOW);
  EXPECT_EQ(decision.action, Action::Fail);
  EXPECT_STREQ(decision.reason, "no protected preroll interval");
  EXPECT_FALSE(preroll.Armed());
  EXPECT_FALSE(preroll.Active());
}
TEST(PlaybackPreroll, WaitsForTheEngineToReportSync)
{
  Preroll preroll;
  ASSERT_NO_FATAL_FAILURE(Start(preroll));

  // Runway and lead are both healthy; the engine has simply not reported sync
  // yet. Releasing here hands the picture over while the sink is still muting
  // its first frames, which is the whole reason the preroll exists.
  ASSERT_TRUE(preroll.SetAudio(GENERATION, Audio(NOW + 1, false), NOW + 1));
  EXPECT_EQ(preroll.Poll(NOW + 1).action, Action::None);
  EXPECT_TRUE(preroll.Active());

  ASSERT_TRUE(preroll.SetAudio(GENERATION, Audio(NOW + 2, true), NOW + 2));
  EXPECT_EQ(preroll.Poll(NOW + 2).action, Action::Release);
}

} // namespace

