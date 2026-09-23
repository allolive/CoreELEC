/*
 *  Copyright (C) 2026 Team CoreELEC
 *  This file is part of CoreELEC - https://coreelec.org
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

// The fence itself is covered separately. What matters here is the wiring: the
// decorator that carries a ticket into Kodi's legacy add-on callback, and the
// real callback queue that may run it, drop it, or never reach it at all. Each
// of those paths has to resolve the ticket exactly once, and a path that
// cannot report completion must not claim it.

#include "cores/IPlayerCallback.h"
#include "interfaces/legacy/AddonClass.h"
#include "interfaces/legacy/CallbackFunction.h"
#include "interfaces/legacy/CallbackHandler.h"
#include "utils/PlaybackCallbackFence.h"

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace
{
using Fence = CPlaybackCallbackFence;
using Status = Fence::Status;

class Receiver : public XBMCAddon::AddonClass
{
public:
  // Protected on the real AddonClass; a test needs to drive recipient teardown.
  using XBMCAddon::AddonClass::deallocating;

  std::function<void()> action = [] {};
  std::atomic<unsigned int> calls{0};
  void Resume()
  {
    ++calls;
    action();
  }
};
using Callback = XBMCAddon::CallbackFunctionWithCompletion<Receiver>;

TEST(PlaybackCallbackDecorator, RunningTheCallbackResolvesTheTicketExactlyOnce)
{
  Receiver receiver;
  Fence fence(19);
  auto callback = std::make_unique<Callback>(&receiver, &Receiver::Resume, fence.Register());
  EXPECT_EQ(&receiver, callback->getObject()) << "decorator changed the recipient identity";

  fence.Seal();
  ASSERT_EQ(Status::PENDING, fence.Read().status) << "merely queuing reported execution";

  callback->executeCallback();
  EXPECT_EQ(Status::COMPLETED, fence.Read().status);
  EXPECT_EQ(1u, receiver.calls);

  callback.reset();
  EXPECT_EQ(1u, fence.Read().returned) << "destruction completed the ticket a second time";
  EXPECT_EQ(0u, fence.Read().discarded) << "destruction completed the ticket a second time";
}

TEST(PlaybackCallbackDecorator, ACallbackCarryingNoTicketStillRuns)
{
  Receiver receiver;
  Callback untracked(&receiver, &Receiver::Resume, {});
  untracked.executeCallback();
  EXPECT_EQ(1u, receiver.calls) << "an untracked callback stopped working";
}

TEST(PlaybackCallbackDecorator, ARecipientThatCannotReportCompletionDoesNotClaimIt)
{
  // An IPlayerCallback that does not take the fence still gets called, but it
  // has no way to say when it finished - so the request must fail rather than
  // be certified on its behalf.
  class Unsupported : public IPlayerCallback
  {
  public:
    unsigned int calls = 0;
    void OnPlayBackEnded() override {}
    void OnPlayBackStarted(const CFileItem&) override {}
    void OnPlayBackStopped() override {}
    void OnPlayBackError() override {}
    void OnQueueNextItem() override {}
    void OnPlayBackResumed() override { ++calls; }
  } callback;

  auto fence = std::make_shared<Fence>(26);
  static_cast<IPlayerCallback&>(callback).OnPlayBackResumed(fence);
  fence->Seal();

  EXPECT_EQ(1u, callback.calls) << "the recipient was not called at all";
  EXPECT_EQ(Status::FAILED, fence->Read().status) << "async completion was falsely certified";
  EXPECT_EQ(1u, fence->Read().discarded);
}

TEST(PlaybackCallbackDecorator, AThrowingCallbackFailsTheFenceAndPropagates)
{
  Receiver receiver;
  receiver.action = [] { throw std::runtime_error("callback failure"); };
  Fence fence(20);
  auto callback = std::make_unique<Callback>(&receiver, &Receiver::Resume, fence.Register());
  fence.Seal();

  EXPECT_THROW(callback->executeCallback(), std::runtime_error) << "the exception was swallowed";
  EXPECT_EQ(Status::FAILED, fence.Read().status) << "a throwing callback left the fence pending";
  EXPECT_EQ(1u, fence.Read().failed);

  callback.reset();
  EXPECT_EQ(1u, fence.Read().failed) << "destruction resolved an already failed ticket";
  EXPECT_EQ(0u, fence.Read().discarded) << "destruction resolved an already failed ticket";
}

TEST(PlaybackCallbackDecorator, DestroyingAnUnrunCallbackFailsRatherThanHangs)
{
  // What actually happens when the queue drops the call or the recipient goes
  // away: the decorator is destroyed without ever running.
  Receiver receiver;
  Fence fence(21);
  auto callback = std::make_unique<Callback>(&receiver, &Receiver::Resume, fence.Register());
  fence.Seal();
  callback.reset();

  EXPECT_EQ(Status::FAILED, fence.Read().status) << "a dropped callback reported success";
  EXPECT_EQ(1u, fence.Read().discarded);
}

TEST(PlaybackCallbackDecorator, ThePlayerSeesPendingWhileTheCallbackIsStillInside)
{
  Receiver receiver;
  Fence fence(22);
  std::promise<void> seekRequested;
  auto seekRequest = seekRequested.get_future();
  std::promise<void> seekProcessed;
  auto seekReply = seekProcessed.get_future().share();
  receiver.action = [&] {
    seekRequested.set_value();
    seekReply.wait();
  };

  auto callback = std::make_unique<Callback>(&receiver, &Receiver::Resume, fence.Register());
  fence.Seal();
  auto worker = std::async(std::launch::async, [&] { callback->executeCallback(); });

  seekRequest.wait();
  EXPECT_EQ(Status::PENDING, fence.Read().status) << "a callback waiting on the player looked done";

  // The player side is free to service the seek meanwhile; this covers the
  // non-blocking completion plumbing, not the FIFO barrier.
  seekProcessed.set_value();
  worker.get();
  EXPECT_EQ(Status::COMPLETED, fence.Read().status);
  EXPECT_EQ(22u, fence.Read().request) << "the original resume request was not the one completed";
}

TEST(PlaybackCallbackDecorator, AStaleCallbackCannotReleaseTheRequestThatReplacedIt)
{
  Fence old(23);
  auto pending = old.Register();
  old.Seal();
  old.Cancel();
  ASSERT_EQ(Status::CANCELLED, old.Read().status) << "cancel was not immediate";
  ASSERT_FALSE(old.Register()) << "a cancelled fence still accepted registration";

  Fence replacement(24);
  auto fresh = replacement.Register();
  replacement.Seal();

  pending->Invoke([] {});
  EXPECT_EQ(Status::CANCELLED, old.Read().status) << "a stale completion un-cancelled the fence";
  EXPECT_EQ(Status::PENDING, replacement.Read().status) << "a stale completion released the new one";

  fresh->Invoke([] {});
  EXPECT_EQ(Status::COMPLETED, replacement.Read().status);
  EXPECT_EQ(24u, replacement.Read().request) << "the replacement completed the wrong request";
}

TEST(PlaybackCallbackDecorator, NoTicketIsLostWhenManyThreadsCompleteAtOnce)
{
  Fence fence(25);
  std::vector<std::shared_ptr<Fence::Ticket>> tickets;
  for (unsigned int i = 0; i < 512; ++i)
    tickets.push_back(fence.Register());
  fence.Seal();

  std::atomic<unsigned int> calls{0};
  std::vector<std::thread> workers;
  for (unsigned int worker = 0; worker < 8; ++worker)
    workers.emplace_back([&, worker] {
      for (size_t i = worker; i < tickets.size(); i += 8)
        tickets[i]->Invoke([&] { ++calls; });
    });
  for (auto& worker : workers)
    worker.join();

  EXPECT_EQ(Status::COMPLETED, fence.Read().status);
  EXPECT_EQ(512u, fence.Read().returned) << "concurrent completion lost a callback";
  EXPECT_EQ(512u, calls);

  tickets.clear();
  EXPECT_EQ(512u, fence.Read().returned) << "releasing completed tickets changed the counts";
  EXPECT_EQ(0u, fence.Read().discarded) << "releasing completed tickets changed the counts";
}

// Kodi's real callback queue. Each of its outcomes has to resolve the ticket.
class CallbackQueue : public testing::Test
{
protected:
  class Handler : public XBMCAddon::RetardedAsyncCallbackHandler
  {
  public:
    bool canRun = true;
    bool isStateOk(XBMCAddon::AddonClass*) override { return canRun; }
    bool shouldRemoveCallback(XBMCAddon::AddonClass*, void* userData) override
    {
      return userData == this;
    }
  };

  XBMCAddon::AddonClass::Ref<Handler> handler{new Handler};
  XBMCAddon::AddonClass::Ref<Receiver> receiver{new Receiver};
};

TEST_F(CallbackQueue, ACallbackWaitsForItsOwnThreadAndThenRuns)
{
  Fence fence(27);
  handler->invokeCallback(new Callback(receiver.get(), &Receiver::Resume, fence.Register()));
  fence.Seal();

  handler->canRun = false;
  Handler::makePendingCalls();
  EXPECT_EQ(Status::PENDING, fence.Read().status) << "ran on the wrong thread";
  EXPECT_EQ(0u, receiver->calls) << "ran on the wrong thread";

  handler->canRun = true;
  Handler::makePendingCalls();
  EXPECT_EQ(Status::COMPLETED, fence.Read().status) << "the queue never ran the callback";
  EXPECT_EQ(1u, receiver->calls);
}

TEST_F(CallbackQueue, RemovingAQueuedCallbackDiscardsItsTicketExactlyOnce)
{
  Fence fence(28);
  handler->invokeCallback(new Callback(receiver.get(), &Receiver::Resume, fence.Register()));
  fence.Seal();

  Handler::clearPendingCalls(nullptr);
  EXPECT_EQ(Status::PENDING, fence.Read().status) << "an unmatched removal discarded the callback";

  Handler::clearPendingCalls(handler.get());
  EXPECT_EQ(Status::FAILED, fence.Read().status);
  EXPECT_EQ(1u, fence.Read().discarded) << "removal did not discard the ticket exactly once";
  EXPECT_EQ(0u, receiver->calls);
}

TEST_F(CallbackQueue, AnExceptionInsideTheQueueStillFailsTheFence)
{
  Fence fence(29);
  receiver->action = [] { throw std::runtime_error("addon failure"); };
  handler->invokeCallback(new Callback(receiver.get(), &Receiver::Resume, fence.Register()));
  fence.Seal();

  Handler::makePendingCalls(); // The real handler catches and logs it.
  EXPECT_EQ(Status::FAILED, fence.Read().status) << "the failure outcome was lost";
  EXPECT_EQ(1u, fence.Read().failed);
}

TEST_F(CallbackQueue, ARecipientTornDownBeforeItsTurnFailsRatherThanRuns)
{
  Fence fence(30);
  handler->invokeCallback(new Callback(receiver.get(), &Receiver::Resume, fence.Register()));
  fence.Seal();

  receiver->deallocating();
  Handler::makePendingCalls();
  EXPECT_EQ(Status::FAILED, fence.Read().status) << "a torn-down recipient left the fence pending";
  EXPECT_EQ(1u, fence.Read().discarded);
  EXPECT_EQ(0u, receiver->calls) << "a torn-down recipient was still called";
}
} // namespace
