// SPDX-License-Identifier: GPL-2.0-or-later
//
// The included bodies come unchanged from the fully patched players. These
// collaborators control delivery and output acceptance; no device is involved.
#include "cores/VideoPlayer/AudioPreroll.h"
#include "cores/VideoPlayer/PlaybackPreroll.h"
#include "cores/VideoPlayer/PlaybackResumeGate.h"
#include "cores/VideoPlayer/VideoRenderers/VideoPrerollGate.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>
using namespace std::chrono_literals;
struct CThread
{
  template<class T>
  static void Sleep(T)
  {
  }
};
constexpr int LOGDEBUG = 0, LOGWARNING = 1;
struct CLog
{
  template<class... T>
  static void Log(T&&...)
  {
  }
};
struct CDVDMsg
{
  enum Type
  {
    AUDIO_PREROLL,
    PLAYER_AUDIO_PREROLL,
    PLAYER_SEEK,
    PLAYER_SEEK_CHAPTER,
    PLAYER_SETSPEED,
    PLAYER_OPENFILE,
    PLAYER_ABORT,
    GENERAL_RESYNC,
    GENERAL_PAUSE
  };
};
template<class T>
struct CDVDMsgType
{
  T m_value;
  CDVDMsgType(CDVDMsg::Type, const T& value) : m_value(value) {}
};
using CDVDMsgDouble = CDVDMsgType<double>;
using CDVDMsgBool = CDVDMsgType<bool>;
struct IDVDStreamPlayer
{
  enum
  {
    SYNC_WAITSYNC,
    SYNC_INSYNC
  };
};
struct CCurrentStream
{
  enum
  {
    AV_SYNC_NONE
  };
  int syncState = IDVDStreamPlayer::SYNC_WAITSYNC;
  int avsync = 0;
  struct
  {
    int codec = 0;
  } hint;
};
constexpr int AV_CODEC_ID_TRUEHD = 1, AV_CODEC_ID_DTS = 2;
struct Clock
{
  double now = 10001000, pts = 500000;
  bool paused = false;
  std::vector<std::string>* events = nullptr;
  double GetAbsoluteClock() const { return now; }
  double GetClock() const { return pts; }
  bool IsPaused() const { return paused; }
  void SetSpeed(int speed)
  {
    paused = speed == DVD_PLAYSPEED_PAUSE;
    if (events && paused)
      events->push_back("clock-pause");
  }
  void Discontinuity(double value)
  {
    pts = value;
    events->push_back("anchor");
  }
};
struct AudioHarness
{
  CAudioPreroll m_preroll;
  struct Sink
  {
    AudioPrerollSinkState state{500000, true, true, true};
    AudioPrerollSinkState GetPrerollSyncState() { return state; }
  } m_audioSink;
  Clock clock;
  Clock* m_pClock = &clock;
  struct Parent
  {
    std::deque<AudioPrerollStatus> reports;
    void Put(std::shared_ptr<CDVDMsgType<AudioPrerollStatus>> p) { reports.push_back(p->m_value); }
  } m_messageParent;
  struct Frame
  {
    int nb_frames = 100;
  } audioframe;
  std::deque<AudioPrerollCommand> commands;
  std::vector<bool> pauses;
  double heard = DVD_NOPTS_VALUE;
  int speed = DVD_PLAYSPEED_NORMAL;
  bool correctionsSuppressed = true, onlyPrioMsgs = false;
  double m_lastPrerollStatusUs = 0;
  int m_syncState = IDVDStreamPlayer::SYNC_INSYNC;
  void ResetPrerollCorrections() { correctionsSuppressed = m_preroll.Active(); }
  void PublishPreroll(AudioPrerollStatus::Phase phase, bool)
  {
    m_messageParent.reports.push_back(m_preroll.Observe(m_audioSink.state, clock.now, phase));
  }
  void SendMessage(std::shared_ptr<CDVDMsgType<AudioPrerollCommand>> message, int)
  {
    commands.push_back(message->m_value);
  }
  void SendMessage(std::shared_ptr<CDVDMsgDouble>, int) {}
  void SendMessage(std::shared_ptr<CDVDMsgBool> message, int) { pauses.push_back(message->m_value); }
  void SetSpeed(int value) { speed = value; }
  double GetHeardPts() const { return heard; }
  void Handle(const AudioPrerollCommand& command);
  bool CanProcessOutput();
  void Deliver()
  {
    ASSERT_FALSE(commands.empty());
    const auto command = commands.front();
    commands.pop_front();
    Handle(command);
  }
};
struct Renderer
{
  CVideoPrerollGate gate;
  std::vector<std::string>* events = nullptr;
  bool acceptsRelease = true;
  bool BeginVideoPreroll(uint64_t generation, double target)
  {
    return gate.Begin(generation, target);
  }
  void CancelVideoPreroll(uint64_t generation) { gate.Cancel(generation); }
  CVideoPrerollGate::Snapshot GetVideoPreroll() { return gate.Read(); }
  bool ReleaseVideoPreroll(uint64_t generation)
  {
    events->push_back("video-release");
    return acceptsRelease && gate.Release(generation);
  }
};
struct CVideoPlayer
{
  CPlaybackPreroll m_resumePreroll;
  CPlaybackResumeGate m_resumeGate;
  Renderer m_renderManager;
  AudioHarness audio;
  AudioHarness* m_VideoPlayerAudio = &audio;
  struct Video
  {
    int speed = DVD_PLAYSPEED_NORMAL;
    void SendMessage(std::shared_ptr<CDVDMsgDouble>, int) {}
    void SetSpeed(int value) { speed = value; }
  } video;
  Video* m_VideoPlayerVideo = &video;
  struct ProcessInfo
  {
    void SetFrameAdvance(bool) {}
    bool GetVideoInterlaced() const { return false; }
    std::optional<double> phase;
    std::optional<double> GetPresentationPhase(double, double) const { return phase; }
  } process;
  ProcessInfo* m_processInfo = &process;
  struct Messenger
  {
    std::set<CDVDMsg::Type> pending;
    int GetPacketCount(CDVDMsg::Type type) { return pending.count(type); }
  } m_messenger;
  struct Timer
  {
    template<class T>
    void Set(T)
    {
    }
  } m_syncTimer;
  struct DisplayTime
  {
    void Clear() {}
  } m_displayTime;
  struct CacheInfo
  {
    double level, offset, time, forwardTime;
    bool endCached, endOfInput, valid;
  } m_srcCache{};
  static constexpr double SRC_UNDERRUN_TIME = 1.0;
  bool m_nextStreamSyncing = false;
  std::vector<std::string> events;
  Clock m_clock;
  CCurrentStream m_CurrentVideo, m_CurrentAudio;
  bool m_seekingResumePreroll = false, m_resumePrerollPaused = false, m_resumePrerollFailed = false;
  bool m_resumeAudioCommitPending = false;
  uint64_t m_resumePictureGeneration = 0;
  std::optional<double> m_resumePrerollTarget, m_syncStartPtsWait;
  double m_resumePrerollReleaseTime = 0;
  std::optional<double> m_startPrerollFrom;
  bool m_startAudioHeld = false, m_startPrerollPending = false, m_displayLost = false;
  std::optional<std::chrono::steady_clock::time_point> m_startPrerollWait;
  bool m_pauseDeferred = false;
  double m_pauseDeferStart = 0.0, m_pauseDeferLastHeard = DVD_NOPTS_VALUE;
  double m_pauseDeferLastAdvance = 0.0, m_pauseDeferLastPoll = 0.0;
  bool startEligible = true;
  int streamsAnnounced = 0;
  int m_playSpeed = 0, m_streamPlayerSpeed = 0, m_caching = 0;
  enum
  {
    CACHESTATE_DONE
  };
  std::string recovery;
  CVideoPlayer()
  {
    m_clock.events = &events;
    m_renderManager.events = &events;
  }
  void PublishPlaySpeed(int, bool) {}
  bool PollResumePreroll();
  double ResumePrerollLeadIn(double rendererLeadUs, double& settle, double& receiverStart) const;
  bool ResumePrerollHasLead(double target, double rendererLeadUs) const;
  bool CanPrepareResumePreroll() const { return true; }
  bool CanPrepareStartPreroll(int) const { return startEligible; }
  void AnnounceStreamsReady() { ++streamsAnnounced; }
  void FinishDeferredPause(bool now);
  void ReleaseStartHold(const char* reason);
  void EndStartPreroll();
  bool IsPassthrough() const { return true; }
  struct State
  {
    double time_offset = 0.0;
  } m_State;
  double m_offset_pts = 0.0;
  void CancelResumePreroll(bool restoreSpeed);
  void FlushBuffers(double pts, bool accurate, [[maybe_unused]] bool sync);
  void HandleAudioPrerollStatus(const AudioPrerollStatus& status);
  // Recovery I/O is outside this suite; the real cancellation body is included.
  void RecoverResumePreroll(const char* reason)
  {
    recovery = reason;
    CancelResumePreroll(false);
  }
};
#include "PrerollPlayerSections.inc"

namespace
{
void Prepare(CVideoPlayer& player, bool armed = false)
{
  constexpr double target = 1000000, now = 10000000;
  player.m_resumeGate.InvalidateOutput();
  const auto gen = player.m_resumeGate.OutputGeneration();
  player.m_resumePictureGeneration = gen;
  player.m_resumePrerollTarget = target;
  ASSERT_TRUE(player.m_renderManager.BeginVideoPreroll(gen, target));
  ASSERT_EQ(gen, player.m_renderManager.gate.ArmAfterFlush());
  ASSERT_TRUE(player.m_renderManager.gate.Retain(gen, target));
  ASSERT_TRUE(player.m_resumePreroll.Begin(gen, target, now));
  if (armed)
    player.m_resumePreroll.ArmWhenPrepared();
  ASSERT_TRUE(player.m_resumePreroll.SetVideoReady(gen, target, now));
  ASSERT_TRUE(
      player.m_resumePreroll.SetAudio(gen, {500000, target, 500000, now, false, true}, now));
  const auto start = player.m_resumePreroll.Poll(now);
  if (armed)
    ASSERT_TRUE(player.m_resumePreroll.Armed());
  else
  {
    ASSERT_EQ(CPlaybackPreroll::Action::StartAudio, start.action);
    ASSERT_TRUE(player.m_resumePreroll.SetAudio(
        gen, {500000, target, 500000, now + 1000, true, true}, now + 1000));
    ASSERT_EQ(CPlaybackPreroll::Action::Release, player.m_resumePreroll.Poll(now + 1000).action);
  }
  ASSERT_TRUE(player.audio.m_preroll.Begin(gen, target));
  ASSERT_TRUE(player.audio.m_preroll.Accepted(500000, 500000, 100, 100, 0));
  ASSERT_TRUE(player.audio.m_preroll.HoldPacket(target, 20000, true, true));
  player.audio.m_preroll.Observe({500000, true, true, false}, now);
}
AudioPrerollStatus Ready(CVideoPlayer& player)
{
  player.audio.Handle(
      {AudioPrerollCommand::Action::PrepareRelease, player.m_resumePictureGeneration, 1000000});
  EXPECT_EQ(1u, player.audio.m_messageParent.reports.size());
  return player.audio.m_messageParent.reports.front();
}
TEST(PlayerPrerollWiring, ReadinessDoesNotReleaseAudioBeforePlayerAcceptsIt)
{
  CVideoPlayer player;
  ASSERT_NO_FATAL_FAILURE(Prepare(player));
  const auto status = Ready(player);
  EXPECT_EQ(AudioPrerollStatus::Phase::ReadyToRelease, status.phase);
  EXPECT_TRUE(status.mainHeld);
  EXPECT_FALSE(player.audio.CanProcessOutput());
  EXPECT_TRUE(player.audio.correctionsSuppressed);
  player.m_startPrerollFrom = 0;
  const double clock = player.m_clock.GetClock();
  player.HandleAudioPrerollStatus(status);
  EXPECT_EQ((std::vector<std::string>{"video-release"}), player.events)
      << "the release leaves the clock where the audio was started on it";
  EXPECT_EQ(clock, player.m_clock.GetClock());
  EXPECT_FALSE(player.m_startPrerollFrom);
  EXPECT_EQ(1, player.streamsAnnounced);
  ASSERT_EQ(1u, player.audio.commands.size());
  EXPECT_EQ(AudioPrerollCommand::Action::End, player.audio.commands.front().action);
  EXPECT_FALSE(player.audio.CanProcessOutput()) << "enqueuing commit is not consuming it";
  player.audio.Deliver();
  EXPECT_TRUE(player.audio.CanProcessOutput());
  EXPECT_FALSE(player.audio.correctionsSuppressed);
  EXPECT_FALSE(player.m_resumePreroll.Active());
}
TEST(PlayerPrerollWiring, AnAudioFailureBeforeCommitRecoversTheVideoHandoff)
{
  CVideoPlayer player;
  ASSERT_NO_FATAL_FAILURE(Prepare(player));
  player.HandleAudioPrerollStatus(Ready(player));
  player.audio.m_preroll.Fail();
  player.audio.Deliver();
  ASSERT_EQ(2u, player.audio.m_messageParent.reports.size());
  player.HandleAudioPrerollStatus(player.audio.m_messageParent.reports.back());
  EXPECT_EQ("audio commit failed", player.recovery);
  ASSERT_EQ(1u, player.audio.commands.size());
  EXPECT_EQ(AudioPrerollCommand::Action::Cancel, player.audio.commands.front().action);
  player.audio.Deliver();
  EXPECT_EQ(0, player.audio.audioframe.nb_frames);
  EXPECT_EQ(0u, player.m_resumePictureGeneration);
}
TEST(PlayerPrerollWiring, RejectedReadinessLeavesAudioAndCorrectionsHeld)
{
  CVideoPlayer player;
  ASSERT_NO_FATAL_FAILURE(Prepare(player));
  auto status = Ready(player);
  status.hostUs -= 100001;
  player.HandleAudioPrerollStatus(status);
  EXPECT_TRUE(player.m_resumePrerollFailed);
  EXPECT_TRUE(player.events.empty());
  EXPECT_TRUE(player.audio.commands.empty());
  EXPECT_FALSE(player.audio.CanProcessOutput());
  EXPECT_TRUE(player.audio.correctionsSuppressed);
}
TEST(PlayerPrerollWiring, RendererRefusalDoesNotCommitAudio)
{
  CVideoPlayer player;
  ASSERT_NO_FATAL_FAILURE(Prepare(player));
  player.m_renderManager.acceptsRelease = false;
  player.HandleAudioPrerollStatus(Ready(player));
  EXPECT_TRUE(player.m_resumePrerollFailed);
  EXPECT_TRUE(player.audio.commands.empty());
  EXPECT_FALSE(player.audio.CanProcessOutput());
  EXPECT_TRUE(player.audio.correctionsSuppressed);
}
TEST(PlayerPrerollWiring, NewPauseOrSeekBeforeAcknowledgementCancelsThePacket)
{
  for (auto command : {CDVDMsg::PLAYER_SETSPEED, CDVDMsg::PLAYER_SEEK, CDVDMsg::PLAYER_ABORT})
  {
    CVideoPlayer player;
    ASSERT_NO_FATAL_FAILURE(Prepare(player));
    auto status = Ready(player);
    player.m_messenger.pending.insert(command);
    player.HandleAudioPrerollStatus(status);
    EXPECT_FALSE(player.recovery.empty());
    EXPECT_TRUE(player.events.empty());
    ASSERT_EQ(1u, player.audio.commands.size());
    EXPECT_EQ(AudioPrerollCommand::Action::Cancel, player.audio.commands.front().action);
    player.audio.Deliver();
    EXPECT_EQ(0, player.audio.audioframe.nb_frames);
    EXPECT_EQ(IDVDStreamPlayer::SYNC_WAITSYNC, player.audio.m_syncState);
  }
}
TEST(PlayerPrerollWiring, OldReadinessCannotReleaseAReplacement)
{
  CVideoPlayer player;
  ASSERT_NO_FATAL_FAILURE(Prepare(player));
  auto status = Ready(player);
  player.CancelResumePreroll(false);
  player.audio.Deliver();
  ASSERT_NO_FATAL_FAILURE(Prepare(player));
  player.HandleAudioPrerollStatus(status);
  EXPECT_TRUE(player.m_resumePreroll.Releasing());
  EXPECT_TRUE(player.events.empty());
  EXPECT_TRUE(player.audio.commands.empty());
}
TEST(PlayerPrerollWiring, CallbackSeeksReplacePreparationButKeepThePictureHold)
{
  CVideoPlayer player;
  ASSERT_NO_FATAL_FAILURE(Prepare(player, true));
  const auto oldGen = player.m_resumePictureGeneration;
  auto fence = player.m_resumeGate.Begin(1000, false, CPlaybackResumeGate::Clock::now());
  auto ticket = fence->Register();
  fence->Seal();
  player.FlushBuffers(2000000, true, true);
  EXPECT_TRUE(player.m_resumeGate.IsHolding());
  EXPECT_FALSE(player.m_resumePreroll.Active());
  EXPECT_TRUE(player.m_renderManager.GetVideoPreroll().holding);
  EXPECT_EQ(2000000, player.m_resumePrerollTarget);
  EXPECT_GT(player.m_resumePictureGeneration, oldGen);
  const auto nextGen = player.m_resumePictureGeneration;
  player.FlushBuffers(3000000, true, true);
  EXPECT_EQ(3000000, player.m_resumePrerollTarget);
  EXPECT_GT(player.m_resumePictureGeneration, nextGen);
  EXPECT_TRUE(player.audio.commands.empty()) << "no bare cancellation of the picture owner";
  ticket->Invoke([] {});
  const auto barrier = player.m_resumeGate.Poll(CPlaybackResumeGate::Clock::now());
  ASSERT_TRUE(barrier);
  ASSERT_TRUE(player.m_resumeGate.Commit(*barrier));
  EXPECT_TRUE(player.m_renderManager.GetVideoPreroll().holding);
}
TEST(PlayerPrerollWiring, AnOrdinarySeekStillCancelsPreparation)
{
  CVideoPlayer player;
  ASSERT_NO_FATAL_FAILURE(Prepare(player));
  player.FlushBuffers(3000000, true, true);
  EXPECT_FALSE(player.m_resumePreroll.Active());
  EXPECT_EQ(0u, player.m_resumePictureGeneration);
  EXPECT_FALSE(player.m_renderManager.GetVideoPreroll().holding);
  EXPECT_FALSE(player.m_resumePrerollTarget);
}
TEST(PlayerPrerollWiring, AnInaccurateCallbackSeekCannotReuseTheOldTarget)
{
  CVideoPlayer player;
  ASSERT_NO_FATAL_FAILURE(Prepare(player, true));
  player.m_resumeGate.Begin(1000, false, CPlaybackResumeGate::Clock::now());
  player.FlushBuffers(DVD_NOPTS_VALUE, false, true);
  EXPECT_TRUE(player.m_renderManager.GetVideoPreroll().holding);
  EXPECT_FALSE(player.m_resumePrerollTarget);
}
TEST(PlayerPrerollWiring, ASubmittedPictureDoesNotRetireAnUncommittedAudioHandoff)
{
  CVideoPlayer player;
  ASSERT_NO_FATAL_FAILURE(Prepare(player));
  player.HandleAudioPrerollStatus(Ready(player));
  const auto gen = player.m_resumePictureGeneration;
  player.m_renderManager.gate.Scheduled();
  player.m_renderManager.gate.Submitted(gen);
  EXPECT_TRUE(player.PollResumePreroll()) << "source reads must wait for the audio commit";
  EXPECT_EQ(gen, player.m_resumePictureGeneration);
  player.audio.Deliver();
  ASSERT_EQ(2u, player.audio.m_messageParent.reports.size());
  EXPECT_EQ(AudioPrerollStatus::Phase::Released, player.audio.m_messageParent.reports.back().phase);
  player.HandleAudioPrerollStatus(player.audio.m_messageParent.reports.back());
  player.PollResumePreroll();
  EXPECT_EQ(0u, player.m_resumePictureGeneration);
}
TEST(PlayerPrerollWiring, AStalledAudioCommitStillTimesOutAfterVideoSubmission)
{
  CVideoPlayer player;
  ASSERT_NO_FATAL_FAILURE(Prepare(player));
  player.HandleAudioPrerollStatus(Ready(player));
  const auto gen = player.m_resumePictureGeneration;
  player.m_renderManager.gate.Scheduled();
  player.m_renderManager.gate.Submitted(gen);
  player.m_clock.now += 2000001;
  EXPECT_TRUE(player.PollResumePreroll());
  EXPECT_FALSE(player.recovery.empty());
  EXPECT_EQ(0u, player.m_resumePictureGeneration);
}
TEST(PlayerPrerollWiring, NoPreparationNearerTheStartThanTheLeadIn)
{
  // TrueHD with the renderer at its 100 ms floor: the 800 ms lead-in and the
  // 106.7 ms settle the box logged when 3D films were paused at 0.4-0.7 s.
  CVideoPlayer player;
  player.m_CurrentAudio.hint.codec = AV_CODEC_ID_TRUEHD;
  double settle = 0, receiverStart = 0;
  EXPECT_DOUBLE_EQ(800000, player.ResumePrerollLeadIn(100000, settle, receiverStart));
  EXPECT_DOUBLE_EQ(350000, receiverStart);
  EXPECT_NEAR(106666.7, settle, 0.1);
  EXPECT_FALSE(player.ResumePrerollHasLead(604000, 100000));
  EXPECT_FALSE(player.ResumePrerollHasLead(906000, 100000));
  EXPECT_TRUE(player.ResumePrerollHasLead(800000 + settle, 100000));
  EXPECT_FALSE(player.ResumePrerollHasLead(1000000, 200000)) << "a longer renderer lead counts";
}
TEST(PlayerPrerollWiring, OtherCodecsNeedNoSettleOrReceiverAllowance)
{
  CVideoPlayer player;
  player.m_CurrentAudio.hint.codec = AV_CODEC_ID_DTS;
  double settle = 1, receiverStart = 1;
  EXPECT_DOUBLE_EQ(450000, player.ResumePrerollLeadIn(100000, settle, receiverStart));
  EXPECT_EQ(0, settle);
  EXPECT_EQ(0, receiverStart);
  EXPECT_FALSE(player.ResumePrerollHasLead(449999, 100000));
  EXPECT_TRUE(player.ResumePrerollHasLead(450000, 100000));
}
void Start(CVideoPlayer& player, std::optional<double> phase)
{
  constexpr double target = 1000000, now = 10000000;
  player.m_processInfo->phase = phase;
  player.m_resumeGate.InvalidateOutput();
  const auto gen = player.m_resumeGate.OutputGeneration();
  player.m_resumePictureGeneration = gen;
  ASSERT_TRUE(player.m_renderManager.BeginVideoPreroll(gen, target));
  ASSERT_EQ(gen, player.m_renderManager.gate.ArmAfterFlush());
  ASSERT_TRUE(player.m_renderManager.gate.Retain(gen, target));
  ASSERT_TRUE(player.m_resumePreroll.Begin(gen, target, now));
  ASSERT_TRUE(
      player.m_resumePreroll.SetAudio(gen, {500000, target, 500000, now, false, true}, now));
}
TEST(PlayerPrerollWiring, TheAudioStartsOnAClockAlignedToTheDisplay)
{
  CVideoPlayer plain, aligned;
  ASSERT_NO_FATAL_FAILURE(Start(plain, std::nullopt));
  ASSERT_NO_FATAL_FAILURE(Start(aligned, 2000.0));
  ASSERT_TRUE(plain.PollResumePreroll());
  ASSERT_TRUE(aligned.PollResumePreroll());
  ASSERT_EQ((std::vector<std::string>{"anchor"}), plain.events);
  ASSERT_EQ((std::vector<std::string>{"anchor"}), aligned.events);
  EXPECT_DOUBLE_EQ(plain.m_clock.GetClock() - 2000.0, aligned.m_clock.GetClock());
  EXPECT_EQ(IDVDStreamPlayer::SYNC_INSYNC, aligned.m_CurrentAudio.syncState);
}
TEST(PlayerPrerollWiring, AStartPreparationAnswersToTheStartConditions)
{
  CVideoPlayer player;
  ASSERT_NO_FATAL_FAILURE(Start(player, std::nullopt));
  player.m_startPrerollFrom = 0;
  player.startEligible = false;
  EXPECT_TRUE(player.PollResumePreroll());
  EXPECT_EQ("stream eligibility changed", player.recovery);
  EXPECT_FALSE(player.m_resumePreroll.Active());
  EXPECT_FALSE(player.m_startPrerollFrom);
}
void DeferPause(CVideoPlayer& player, double heardAhead)
{
  player.m_pauseDeferred = true;
  player.m_pauseDeferStart = player.m_clock.now - DVD_SEC_TO_TIME(1);
  player.audio.heard = player.m_clock.GetClock() + heardAhead;
}
TEST(PlayerPrerollWiring, ADeferredPauseStopsTheClockWhereTheAudioWasHeard)
{
  for (double ahead : {-DVD_MSEC_TO_TIME(499), DVD_MSEC_TO_TIME(200)})
  {
    CVideoPlayer player;
    DeferPause(player, ahead);
    player.FinishDeferredPause(false);
    EXPECT_FALSE(player.m_pauseDeferred);
    EXPECT_EQ(player.audio.heard, player.m_clock.GetClock());
    EXPECT_EQ((std::vector<std::string>{"anchor", "clock-pause"}), player.events);
    EXPECT_EQ(DVD_PLAYSPEED_PAUSE, player.video.speed);
    EXPECT_EQ(DVD_PLAYSPEED_PAUSE, player.audio.speed);
    EXPECT_EQ((std::vector<bool>{false}), player.audio.pauses);
  }
}
TEST(PlayerPrerollWiring, ADeferredPauseDoesNotFollowAnImplausibleHeardPosition)
{
  for (double ahead : {-DVD_MSEC_TO_TIME(500), DVD_MSEC_TO_TIME(600)})
  {
    CVideoPlayer player;
    const double clock = player.m_clock.GetClock();
    DeferPause(player, ahead);
    player.FinishDeferredPause(false);
    EXPECT_FALSE(player.m_pauseDeferred);
    EXPECT_EQ(clock, player.m_clock.GetClock());
    EXPECT_EQ((std::vector<std::string>{"clock-pause"}), player.events);
  }
  CVideoPlayer silent;
  DeferPause(silent, 0);
  silent.audio.heard = DVD_NOPTS_VALUE;
  silent.FinishDeferredPause(false);
  EXPECT_FALSE(silent.m_pauseDeferred);
  EXPECT_EQ((std::vector<std::string>{"clock-pause"}), silent.events);
}
TEST(PlayerPrerollWiring, ADeferredPauseWaitsForTheQueuedAudioToPlayOut)
{
  CVideoPlayer player;
  DeferPause(player, -DVD_MSEC_TO_TIME(300));
  player.m_pauseDeferStart = player.m_clock.now;
  player.FinishDeferredPause(false);
  EXPECT_TRUE(player.m_pauseDeferred) << "the heard position is still advancing";
  player.m_clock.now += DVD_MSEC_TO_TIME(40);
  player.audio.heard += DVD_MSEC_TO_TIME(40);
  player.FinishDeferredPause(false);
  EXPECT_TRUE(player.m_pauseDeferred);
  player.m_clock.now += DVD_MSEC_TO_TIME(79);
  player.FinishDeferredPause(false);
  EXPECT_TRUE(player.m_pauseDeferred) << "still for less than 80 ms";
  player.m_clock.now += DVD_MSEC_TO_TIME(5);
  player.FinishDeferredPause(false);
  EXPECT_TRUE(player.m_pauseDeferred) << "polled at most every 10 ms";
  player.m_clock.now += DVD_MSEC_TO_TIME(6);
  player.FinishDeferredPause(false);
  EXPECT_FALSE(player.m_pauseDeferred);
  EXPECT_EQ(player.audio.heard, player.m_clock.GetClock());
  EXPECT_TRUE(player.m_clock.IsPaused());
}
TEST(PlayerPrerollWiring, AFlushFinishesADeferredPauseAtOnce)
{
  CVideoPlayer player;
  const double clock = player.m_clock.GetClock();
  DeferPause(player, -DVD_MSEC_TO_TIME(200));
  player.m_pauseDeferStart = player.m_clock.now;
  player.FlushBuffers(3000000, true, true);
  EXPECT_FALSE(player.m_pauseDeferred);
  EXPECT_TRUE(player.m_clock.IsPaused());
  EXPECT_EQ(clock, player.m_clock.GetClock()) << "the seek moves the clock, not the pause";
  EXPECT_EQ((std::vector<std::string>{"clock-pause"}), player.events);
  EXPECT_EQ(DVD_PLAYSPEED_PAUSE, player.audio.speed);
}
TEST(PlayerPrerollWiring, ASeekReleasesTheHeldStartAudio)
{
  for (bool displayLost : {false, true})
  {
    CVideoPlayer player;
    player.m_startAudioHeld = true;
    player.m_startPrerollPending = true;
    player.m_startPrerollWait = std::chrono::steady_clock::now();
    player.m_displayLost = displayLost;
    player.FlushBuffers(3000000, true, true);
    EXPECT_FALSE(player.m_startAudioHeld);
    EXPECT_FALSE(player.m_startPrerollPending);
    EXPECT_FALSE(player.m_startPrerollWait);
    EXPECT_EQ((std::vector<bool>{displayLost}), player.audio.pauses)
        << "a lost display keeps the audio paused";
  }
  CVideoPlayer ordinary;
  ordinary.FlushBuffers(3000000, true, true);
  EXPECT_TRUE(ordinary.audio.pauses.empty());
}
} // namespace
