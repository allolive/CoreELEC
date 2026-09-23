/*
 *  Copyright (C) 2026 Team CoreELEC
 *  This file is part of CoreELEC - https://coreelec.org
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

// The gate is exercised against Kodi's real message queue rather than a
// stand-in for it, because the ordering under test is the queue's: a barrier
// must never overtake a seek the callback put there first.

#include "cores/VideoPlayer/DVDDemuxers/DVDDemuxUtils.h"
#include "cores/VideoPlayer/DVDMessageQueue.h"
#include "cores/VideoPlayer/PlaybackResumeGate.h"

#include <atomic>
#include <future>

#include <gtest/gtest.h>

// The messages under test never carry a demux payload. Defining the deleter
// here links the real message classes without the demuxer behind them, and
// refuses anything that would genuinely need FFmpeg to free it.
void CDVDDemuxUtils::FreeDemuxPacket(DemuxPacket* packet)
{
  ASSERT_TRUE(!packet || (!packet->pData && !packet->pSideData && !packet->cryptoInfo))
      << "a test allocated a demux payload this seam cannot free";
  delete packet;
}

namespace
{
using Gate = CPlaybackResumeGate;
using Fence = CPlaybackCallbackFence;
using namespace std::chrono_literals;

const Gate::Clock::time_point NOW{123s};

std::shared_ptr<CDVDMsg> Pop(CDVDMessageQueue& queue)
{
  std::shared_ptr<CDVDMsg> message;
  EXPECT_EQ(MSGQ_OK, queue.Get(message, 0ms)) << "expected a player message";
  return message;
}

// What VideoPlayer does when the gate asks for one: put the barrier on the
// same FIFO the callbacks are using.
uint64_t QueueBarrier(Gate& gate, CDVDMessageQueue& queue)
{
  const auto request = gate.Poll(NOW);
  EXPECT_TRUE(request.has_value()) << "completed callback did not request a barrier";
  EXPECT_EQ(MSGQ_OK, queue.Put(std::make_shared<CDVDMsgType<uint64_t>>(
                                   CDVDMsg::PLAYER_RESUME_BARRIER, *request), 0));
  return request.value_or(0);
}

TEST(PlaybackResumeGate, AnIdleGateHoldsNothingAndReleasesNothing)
{
  Gate gate;
  EXPECT_FALSE(gate.IsHolding());
  EXPECT_FALSE(gate.Poll(NOW));
  EXPECT_FALSE(gate.Commit(1));
}

TEST(PlaybackResumeGate, HoldsUntilEveryRecipientHasReturnedAndTheBarrierArrives)
{
  Gate gate;
  auto fence = gate.Begin(1000, false, NOW);
  const auto request = fence->Read().request;
  ASSERT_TRUE(gate.IsHolding()) << "hold not installed before dispatch";
  EXPECT_TRUE(gate.IsCurrentRequest(request));

  auto ticket = fence->Register();
  ticket->Invoke([] {});
  EXPECT_FALSE(gate.Poll(NOW)) << "callback return ended unsealed registration";

  auto later = fence->Register();
  fence->Seal();
  EXPECT_FALSE(gate.Poll(NOW)) << "pending recipient was bypassed";
  EXPECT_FALSE(gate.Commit(request));

  later->Invoke([] {});
  EXPECT_EQ(request, gate.Poll(NOW));
  EXPECT_TRUE(gate.IsHolding()) << "poll released output before the barrier";
  EXPECT_FALSE(gate.Poll(NOW)) << "duplicate barrier requested";

  const auto release = gate.Commit(request);
  ASSERT_TRUE(release) << "completed barrier did not release";
  EXPECT_EQ(1000, release->speed);
  EXPECT_FALSE(release->tempo);
  EXPECT_EQ(Gate::Outcome::COMPLETED, release->outcome);
  EXPECT_FALSE(gate.IsHolding());
  EXPECT_FALSE(gate.Commit(request)) << "barrier released twice";
}

TEST(PlaybackResumeGate, WithNoRecipientsItCompletesAtOnceAndKeepsTheSpeedPayload)
{
  Gate gate;
  auto fence = gate.Begin(1250, true, NOW);
  fence->Seal();

  const auto request = gate.Poll(NOW);
  ASSERT_TRUE(request.has_value()) << "zero recipients needlessly waited";

  const auto release = gate.Commit(*request);
  ASSERT_TRUE(release);
  EXPECT_EQ(1250, release->speed) << "gate changed the speed payload";
  EXPECT_TRUE(release->tempo);
}

TEST(PlaybackResumeGate, TheBarrierQueuesBehindWhatTheCallbackAlreadyPut)
{
  Gate gate;
  CDVDMessageQueue queue("resume-gate-test");
  queue.Init();
  auto fence = gate.Begin(1000, false, NOW);
  auto ticket = fence->Register();
  fence->Seal();
  ticket->Invoke([&] {
    queue.Put(std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_SEEK));
    queue.Put(std::make_shared<CDVDMsg>(CDVDMsg::GENERAL_SYNCHRONIZE));
  });

  const auto request = QueueBarrier(gate, queue);
  EXPECT_TRUE(Pop(queue)->IsType(CDVDMsg::PLAYER_SEEK)) << "barrier overtook the callback's seek";
  gate.InvalidateOutput();
  EXPECT_TRUE(gate.IsCurrentRequest(request)) << "seek cancelled its own resume request";
  EXPECT_TRUE(Pop(queue)->IsType(CDVDMsg::GENERAL_SYNCHRONIZE))
      << "barrier overtook the seek acknowledgement";

  auto message = Pop(queue);
  ASSERT_TRUE(message->IsType(CDVDMsg::PLAYER_RESUME_BARRIER)) << "missing completion barrier";
  const auto release =
      gate.Commit(std::static_pointer_cast<CDVDMsgType<uint64_t>>(message)->m_value);
  ASSERT_TRUE(release);
  EXPECT_EQ(request, release->request);
  EXPECT_EQ(2u, release->generation) << "callback seek did not select the latest output generation";
}

TEST(PlaybackResumeGate, ACallbackBlockedOnItsOwnSeekIsNotTreatedAsComplete)
{
  Gate gate;
  CDVDMessageQueue queue("resume-callback-seek");
  queue.Init();
  auto fence = gate.Begin(1000, false, NOW);
  auto ticket = fence->Register();
  fence->Seal();

  std::atomic<bool> abort{false};
  std::promise<void> submitted;
  auto submittedFuture = submitted.get_future();
  auto callback = std::async(std::launch::async, [&] {
    ticket->Invoke([&] {
      queue.Put(std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_SEEK));
      auto sync = std::make_shared<CDVDMsgGeneralSynchronize>(2s, SYNCSOURCE_PLAYER);
      queue.Put(sync);
      submitted.set_value();
      sync->Wait(abort, 0);
    });
  });

  ASSERT_EQ(std::future_status::ready, submittedFuture.wait_for(1s)) << "callback did not submit";
  EXPECT_FALSE(gate.Poll(NOW)) << "callback waiting for its seek was treated as complete";
  EXPECT_TRUE(gate.IsHolding());
  EXPECT_TRUE(Pop(queue)->IsType(CDVDMsg::PLAYER_SEEK)) << "seek missing from the player FIFO";

  gate.InvalidateOutput();
  auto sync = std::static_pointer_cast<CDVDMsgGeneralSynchronize>(Pop(queue));
  EXPECT_TRUE(sync->Wait(0ms, SYNCSOURCE_PLAYER)) << "player could not acknowledge the seek";
  ASSERT_EQ(std::future_status::ready, callback.wait_for(1s)) << "callback/player seek deadlock";
  callback.get();

  const auto request = QueueBarrier(gate, queue);
  EXPECT_TRUE(Pop(queue)->IsType(CDVDMsg::PLAYER_RESUME_BARRIER));
  EXPECT_TRUE(gate.Commit(request).has_value()) << "synchronous seek did not complete the resume";
}

TEST(PlaybackResumeGate, ADeferredBarrierStillLandsBehindTheFinalSeekTarget)
{
  Gate gate;
  CDVDMessageQueue queue("resume-seek-coalescing");
  queue.Init();
  auto fence = gate.Begin(1000, false, NOW);
  auto ticket = fence->Register();
  fence->Seal();
  ticket->Invoke([&] { queue.Put(std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_SEEK)); });
  const auto request = QueueBarrier(gate, queue);

  // A newer seek can arrive after the callback completes, before the queue drains.
  queue.Put(std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_SEEK));
  EXPECT_TRUE(Pop(queue)->IsType(CDVDMsg::PLAYER_SEEK));
  ASSERT_EQ(1, queue.GetPacketCount(CDVDMsg::PLAYER_SEEK))
      << "fixture did not expose a coalesced seek across the barrier";

  // VideoPlayer coalesces the first seek, so its barrier must defer behind the
  // one still queued.
  auto barrier = Pop(queue);
  ASSERT_TRUE(barrier->IsType(CDVDMsg::PLAYER_RESUME_BARRIER)) << "wrong barrier";
  EXPECT_TRUE(gate.IsCurrentRequest(request)) << "premature release";
  queue.Put(barrier, 0);

  EXPECT_TRUE(Pop(queue)->IsType(CDVDMsg::PLAYER_SEEK))
      << "deferred barrier overtook final target selection";
  EXPECT_TRUE(gate.IsHolding());
  gate.InvalidateOutput();
  EXPECT_TRUE(Pop(queue)->IsType(CDVDMsg::PLAYER_RESUME_BARRIER)) << "deferred barrier lost";
  EXPECT_TRUE(gate.Commit(request).has_value()) << "latest seek could not complete the request";
}

TEST(PlaybackResumeGate, ALateCallbackCannotDisturbTheRequestThatReplacedIt)
{
  Gate gate;
  auto oldFence = gate.Begin(1000, false, NOW);
  auto oldTicket = oldFence->Register();
  oldFence->Seal();
  const auto oldRequest = oldFence->Read().request;

  gate.Cancel();
  auto newFence = gate.Begin(1000, false, NOW);
  const auto newRequest = newFence->Read().request;
  EXPECT_NE(oldRequest, newRequest) << "replacement reused the request";
  EXPECT_FALSE(gate.IsCurrentRequest(oldRequest));

  oldTicket->Invoke([&] { gate.InvalidateOutput(); });
  EXPECT_EQ(Fence::Status::CANCELLED, oldFence->Read().status);
  EXPECT_FALSE(gate.Poll(NOW)) << "late callback completed the replacement";
  EXPECT_FALSE(gate.Commit(oldRequest));
  EXPECT_TRUE(gate.IsCurrentRequest(newRequest));

  newFence->Seal();
  EXPECT_EQ(newRequest, gate.Poll(NOW)) << "replacement completion was lost";
  gate.Cancel();
  EXPECT_FALSE(gate.Commit(newRequest)) << "queued barrier survived pause/stop";
  EXPECT_FALSE(gate.IsHolding());
}

TEST(PlaybackResumeGate, ADiscardedTicketFailsRatherThanReportsSuccess)
{
  Gate gate;
  auto fence = gate.Begin(1000, false, NOW);
  auto ticket = fence->Register();
  fence->Seal();
  ticket.reset();

  const auto request = gate.Poll(NOW);
  ASSERT_TRUE(request);
  EXPECT_EQ(Gate::Outcome::FAILED, gate.Commit(*request)->outcome)
      << "discarded callback falsely reported successful preparation";
}

TEST(PlaybackResumeGate, ATimeoutFiresOnItsDeadlineAndALateReturnCannotUpgradeIt)
{
  Gate gate;
  auto fence = gate.Begin(1000, false, NOW, 250ms);
  auto ticket = fence->Register();
  fence->Seal();

  EXPECT_FALSE(gate.Poll(NOW + 249ms)) << "timeout fired before the deadline";
  const auto request = gate.Poll(NOW + 250ms);
  ASSERT_TRUE(request);
  EXPECT_TRUE(gate.IsHolding()) << "timeout released output before the barrier";
  EXPECT_EQ(Fence::Status::CANCELLED, fence->Read().status)
      << "timeout left callback certification live";

  ticket->Invoke([] {});
  const auto release = gate.Commit(*request);
  ASSERT_TRUE(release);
  EXPECT_EQ(Gate::Outcome::TIMED_OUT, release->outcome) << "late return upgraded a timeout";
}

TEST(PlaybackResumeGate, RegisteringAfterCompletionIsRefusedAndFailsTheRequest)
{
  Gate gate;
  auto fence = gate.Begin(1000, false, NOW);
  fence->Seal();
  const auto request = gate.Poll(NOW);
  ASSERT_TRUE(request);

  EXPECT_FALSE(fence->Register()) << "late registration was accepted";
  EXPECT_EQ(Gate::Outcome::FAILED, gate.Commit(*request)->outcome)
      << "late registration retained a successful result";
}

TEST(PlaybackResumeGate, RequestAndGenerationKeepRisingAcrossManyCycles)
{
  Gate gate;
  uint64_t lastRequest = 0;
  uint64_t lastGeneration = 0;
  for (unsigned int i = 0; i < 2000; ++i)
  {
    auto fence = gate.Begin(1000, false, NOW);
    auto ticket = fence->Register();
    fence->Seal();
    for (unsigned int seek = 0; seek < i % 5; ++seek)
      gate.InvalidateOutput();
    ticket->Invoke([] {});

    const auto request = gate.Poll(NOW);
    ASSERT_TRUE(request);
    const auto release = gate.Commit(*request);
    ASSERT_TRUE(release);
    ASSERT_GT(release->request, lastRequest) << "request identity reused at cycle " << i;
    ASSERT_GT(release->generation, lastGeneration) << "generation reused at cycle " << i;
    lastRequest = release->request;
    lastGeneration = release->generation;
  }
}

TEST(PlaybackResumeGate, ACallbackOutlivingTheGateHoldsNoPermission)
{
  std::shared_ptr<Fence> fence;
  std::shared_ptr<Fence::Ticket> ticket;
  {
    Gate gate;
    fence = gate.Begin(1000, false, NOW);
    ticket = fence->Register();
    fence->Seal();
  }
  ticket->Invoke([] {});
  EXPECT_EQ(Fence::Status::CANCELLED, fence->Read().status)
      << "callback survived owner teardown with live permission";
}
} // namespace
