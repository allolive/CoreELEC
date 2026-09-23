/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "utils/PlaybackCallbackFence.h"

#include <future>
#include <stdexcept>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace
{
using Fence = CPlaybackCallbackFence;
using Status = Fence::Status;

// The work a ticket runs. How often it runs, and whether it runs at all, are
// the properties these cases are about - which a mock states outright instead
// of counting by hand afterwards.
class MockWork
{
public:
  MOCK_METHOD(void, Run, ());
};

constexpr uint64_t REQUEST = 42;
} // namespace

TEST(PlaybackCallbackFence, IsRegisteringUntilItIsSealed)
{
  Fence fence(REQUEST);
  const auto before = fence.Read();
  EXPECT_EQ(before.request, REQUEST);
  EXPECT_EQ(before.status, Status::REGISTERING);

  // An unsealed fence is not pending, however many tickets it has handed out.
  const auto ticket = fence.Register();
  ASSERT_NE(ticket, nullptr);
  EXPECT_EQ(fence.Read().status, Status::REGISTERING);
}

TEST(PlaybackCallbackFence, RefusesRegistrationOnceSealed)
{
  Fence fence(REQUEST);
  fence.Seal();
  EXPECT_EQ(fence.Register(), nullptr);
  // A recipient that arrived too late means the barrier cannot be trusted.
  EXPECT_EQ(fence.Read().status, Status::FAILED);
}

TEST(PlaybackCallbackFence, RunsATicketsWorkExactlyOnce)
{
  ::testing::StrictMock<MockWork> work;
  EXPECT_CALL(work, Run()).Times(1);

  Fence fence(REQUEST);
  auto ticket = fence.Register();
  fence.Seal();
  EXPECT_TRUE(ticket->Invoke([&] { work.Run(); }));
  // A second invocation is refused rather than run again.
  EXPECT_FALSE(ticket->Invoke([&] { work.Run(); }));
}

TEST(PlaybackCallbackFence, CompletesOnceEveryTicketHasReturned)
{
  ::testing::StrictMock<MockWork> first;
  ::testing::StrictMock<MockWork> second;
  EXPECT_CALL(first, Run()).Times(1);
  EXPECT_CALL(second, Run()).Times(1);

  Fence fence(REQUEST);
  auto one = fence.Register();
  auto two = fence.Register();
  fence.Seal();
  EXPECT_EQ(fence.Read().status, Status::PENDING);

  one->Invoke([&] { first.Run(); });
  EXPECT_EQ(fence.Read().status, Status::PENDING);
  two->Invoke([&] { second.Run(); });

  const auto snapshot = fence.Read();
  EXPECT_EQ(snapshot.status, Status::COMPLETED);
  EXPECT_EQ(snapshot.returned, 2u);
  EXPECT_EQ(snapshot.pending, 0u);
}

TEST(PlaybackCallbackFence, ATicketDroppedWithoutRunningFailsTheFence)
{
  ::testing::StrictMock<MockWork> work;
  // Nothing may run: the point is that the ticket goes away unused.
  EXPECT_CALL(work, Run()).Times(0);

  Fence fence(REQUEST);
  {
    auto ticket = fence.Register();
    fence.Seal();
  }
  const auto snapshot = fence.Read();
  EXPECT_EQ(snapshot.status, Status::FAILED);
  EXPECT_EQ(snapshot.discarded, 1u);
}

TEST(PlaybackCallbackFence, RecordsAThrowingCallbackAndLetsItPropagate)
{
  ::testing::StrictMock<MockWork> work;
  EXPECT_CALL(work, Run()).WillOnce(::testing::Throw(std::runtime_error("callback")));

  Fence fence(REQUEST);
  auto ticket = fence.Register();
  fence.Seal();
  // Kodi's own handler still sees the exception; the fence records it first.
  EXPECT_THROW(ticket->Invoke([&] { work.Run(); }), std::runtime_error);

  const auto snapshot = fence.Read();
  EXPECT_EQ(snapshot.status, Status::FAILED);
  EXPECT_EQ(snapshot.failed, 1u);
}

TEST(PlaybackCallbackFence, CancellationStandsWhateverTheTicketsDo)
{
  ::testing::StrictMock<MockWork> work;
  // A ticket already handed out still runs: cancellation withdraws permission
  // to act on the result, it does not reach into a callback in flight.
  EXPECT_CALL(work, Run()).Times(1);
  Fence fence(REQUEST);
  auto ticket = fence.Register();
  fence.Cancel();

  EXPECT_EQ(fence.Register(), nullptr);
  ticket->Invoke([&] { work.Run(); });
  // A result that arrives after cancellation cannot uncancel it.
  EXPECT_EQ(fence.Read().status, Status::CANCELLED);
}

TEST(PlaybackCallbackFence, RunsOnceWhenTwoThreadsInvokeTheSameTicket)
{
  ::testing::StrictMock<MockWork> work;
  EXPECT_CALL(work, Run()).Times(1);

  Fence fence(REQUEST);
  auto ticket = fence.Register();
  fence.Seal();

  auto first = std::async(std::launch::async, [&] { return ticket->Invoke([&] { work.Run(); }); });
  auto second = std::async(std::launch::async, [&] { return ticket->Invoke([&] { work.Run(); }); });
  // Exactly one of the two wins the exchange.
  EXPECT_NE(first.get(), second.get());
  EXPECT_EQ(fence.Read().returned, 1u);
}

TEST(PlaybackCallbackFence, CompletesWhenThereAreNoRecipients)
{
  // A resume with no add-on registered has nothing to wait for and must not
  // leave the player waiting.
  Fence fence(REQUEST);
  fence.Seal();
  EXPECT_EQ(fence.Read().status, Status::COMPLETED);
}

TEST(PlaybackCallbackFence, ARecipientFinishingEarlyDoesNotCloseTheBroadcast)
{
  ::testing::StrictMock<MockWork> first;
  ::testing::StrictMock<MockWork> second;
  EXPECT_CALL(first, Run()).Times(1);
  EXPECT_CALL(second, Run()).Times(1);

  Fence fence(REQUEST);
  auto one = fence.Register();
  one->Invoke([&] { first.Run(); });
  // One recipient finishing before the rest have been offered a ticket must
  // not read as the whole broadcast being done.
  EXPECT_EQ(fence.Read().status, Status::REGISTERING);

  auto two = fence.Register();
  fence.Seal();
  EXPECT_EQ(fence.Read().status, Status::PENDING);
  two->Invoke([&] { second.Run(); });
  EXPECT_EQ(fence.Read().status, Status::COMPLETED);
}

TEST(PlaybackCallbackFence, CountsATicketOnceWhenItRanAndIsThenReleased)
{
  ::testing::StrictMock<MockWork> work;
  EXPECT_CALL(work, Run()).Times(1);

  Fence fence(REQUEST);
  {
    auto ticket = fence.Register();
    fence.Seal();
    ticket->Invoke([&] { work.Run(); });
    EXPECT_EQ(fence.Read().status, Status::COMPLETED);
  }
  // Production reads the fence after the tickets have gone. Destroying one
  // that already ran must not count it a second time: the pending count is
  // unsigned, so a second decrement wraps it and the fence reads PENDING for
  // ever - the resume would never be released.
  const auto snapshot = fence.Read();
  EXPECT_EQ(snapshot.status, Status::COMPLETED);
  EXPECT_EQ(snapshot.pending, 0u);
  EXPECT_EQ(snapshot.discarded, 0u);
}
