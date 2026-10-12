/*
 *  Copyright (C) 2026 Team CoreELEC
 *  This file is part of CoreELEC - https://coreelec.org
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

// A stream's drain - CloseStream(true) waiting for its queue to play out - and
// the stop that has to be able to end it (01_07). Kodi's real message queue
// and synchronise message, so the wait under test is the one the player has.

#include "cores/VideoPlayer/DVDDemuxers/DVDDemuxUtils.h"
#include "cores/VideoPlayer/DVDMessageQueue.h"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include <gtest/gtest.h>

// The messages under test never carry a demux payload.
void CDVDDemuxUtils::FreeDemuxPacket(DemuxPacket* packet)
{
  ASSERT_TRUE(!packet || (!packet->pData && !packet->pSideData && !packet->cryptoInfo))
      << "a test allocated a demux payload this seam cannot free";
  delete packet;
}

namespace
{
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

// What a stream thread does with its queue: take each message and, for the
// drain's synchronise message, report it reached.
void Consume(CDVDMessageQueue& queue, std::atomic<bool>& stop)
{
  while (!stop)
  {
    std::shared_ptr<CDVDMsg> message;
    if (queue.Get(message, 10ms) != MSGQ_OK)
      continue;
    if (message->IsType(CDVDMsg::GENERAL_SYNCHRONIZE))
      std::static_pointer_cast<CDVDMsgGeneralSynchronize>(message)->Wait(0ms, SYNCSOURCE_AUDIO);
  }
}

// A queue holding work the stream thread never gets through - a title queued
// in full whose drain would otherwise run to its 40 s timeout.
void Backlog(CDVDMessageQueue& queue)
{
  queue.Init();
  ASSERT_EQ(MSGQ_OK, queue.Put(std::make_shared<CDVDMsg>(CDVDMsg::GENERAL_RESYNC)));
}

TEST(MessageQueueDrain, ADrainEndsWhenTheStreamReachesIt)
{
  CDVDMessageQueue queue("audio");
  queue.Init();
  std::atomic<bool> stop{false};
  std::thread stream(Consume, std::ref(queue), std::ref(stop));

  EXPECT_TRUE(queue.WaitUntilEmpty());

  stop = true;
  stream.join();
}

TEST(MessageQueueDrain, AbortDrainEndsADrainInProgress)
{
  CDVDMessageQueue queue("audio");
  Backlog(queue);

  auto drain = std::async(std::launch::async, [&queue] {
    const auto start = Clock::now();
    const bool ranToEnd = queue.WaitUntilEmpty();
    return std::make_pair(ranToEnd, Clock::now() - start);
  });
  std::this_thread::sleep_for(200ms);
  queue.AbortDrain();

  ASSERT_EQ(std::future_status::ready, drain.wait_for(2s))
      << "the drain did not end on AbortDrain";
  const auto [ranToEnd, waited] = drain.get();
  EXPECT_FALSE(ranToEnd);
  EXPECT_LT(waited, 1s);
  // nothing was flushed: the stream still has its work
  EXPECT_EQ(MSGQ_OK, [&queue] {
    std::shared_ptr<CDVDMsg> message;
    return queue.Get(message, 0ms);
  }());
}

TEST(MessageQueueDrain, AnAbortSetBeforeTheDrainEndsItAtOnce)
{
  CDVDMessageQueue queue("video");
  Backlog(queue);
  queue.AbortDrain();

  const auto start = Clock::now();
  EXPECT_FALSE(queue.WaitUntilEmpty());
  EXPECT_LT(Clock::now() - start, 50ms);
}

TEST(MessageQueueDrain, AnAbortedQueueStillEndsItsDrain)
{
  CDVDMessageQueue queue("audio");
  Backlog(queue);

  auto drain = std::async(std::launch::async, [&queue] { return queue.WaitUntilEmpty(); });
  std::this_thread::sleep_for(100ms);
  queue.Abort();

  ASSERT_EQ(std::future_status::ready, drain.wait_for(2s));
  EXPECT_FALSE(drain.get());
}

TEST(MessageQueueDrain, ReopeningTheQueueClearsAnAbortedDrain)
{
  CDVDMessageQueue queue("audio");
  Backlog(queue);
  queue.AbortDrain();
  queue.End();

  // the next title opens the stream again; its drain must run in full
  queue.Init();
  std::atomic<bool> stop{false};
  std::thread stream(Consume, std::ref(queue), std::ref(stop));

  EXPECT_TRUE(queue.WaitUntilEmpty());

  stop = true;
  stream.join();
}

} // namespace
