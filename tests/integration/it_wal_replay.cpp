#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>

#include "FakeAddressBook.h"
#include "FakeClock.h"
#include "FakeTicketStore.h"
#include "FakeUiNotifier.h"
#include "IntegrationHarness.h"
#include "aid/controllers/CallController.h"
#include "aid/crosscutting/Config.h"
#include "aid/crosscutting/CorrelationId.h"
#include "aid/crosscutting/Logger.h"
#include "aid/infrastructure/Mailbox.h"
#include "aid/infrastructure/Wal.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/usecases/HandleIncomingCall.h"
#include "aid/value-types/CallEvent.h"
#include "aid/value-types/Contact.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/Ticket.h"

namespace {

using aid::CallId;
using aid::Contact;
using aid::IncomingCall;
using aid::PhoneNumber;
using aid::ProjectId;
using aid::TicketId;
using aid::controllers::CallController;
using aid::crosscutting::Config;
using aid::crosscutting::CorrelationId;
using aid::crosscutting::Logger;
using aid::fakes::FakeAddressBook;
using aid::fakes::FakeClock;
using aid::fakes::FakeTicketStore;
using aid::fakes::FakeUiNotifier;
using aid::infrastructure::Mailbox;
using aid::infrastructure::Wal;
using aid::plumbing::Result;
using aid::plumbing::Task;
using aid::tests::integration::LoggerOnce;
using aid::tests::integration::LoopThread;
using aid::tests::integration::waitUntil;
using aid::usecases::HandleIncomingCall;

constexpr const char* kIncomingJson =
    R"({"event":"Incoming Call","remote":"+491701234567","callid":"replay-1","dialed":"+4930"})";

// WAL durability round-trip: records written by a crashed-then-restarted
// daemon must replay through the same handler chain as live POSTs, and the
// WAL must be truncated once each handler succeeds. Replay happens before
// listeners open, and idempotency comes through at-least-once delivery.
class WalReplayE2E : public ::testing::Test {
protected:
    void SetUp() override {
        const auto pid = static_cast<std::uint64_t>(::getpid());
        static std::atomic<std::uint64_t> counter{0};
        const auto n = counter.fetch_add(1, std::memory_order_relaxed);
        dir_ = std::filesystem::temp_directory_path() /
               ("aid_wal_replay_" + std::to_string(pid) + "_" + std::to_string(n));
        std::filesystem::create_directories(dir_);
        walPath_ = (dir_ / "inbox.log").string();
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    LoggerOnce loggerInit_{};
    FakeClock clock_;
    FakeClock fakeClock_;
    CorrelationId cid_;
    Config::TicketRouting cfg_{ProjectId{"FB"}, "Incognito Caller"};
    std::filesystem::path dir_;
    std::string walPath_;
};

} // namespace

// Phase 1 (write) and Phase 2 (re-open) operate on the same on-disk file.
// The fresh Wal instance must rediscover every record written by the
// previous one.
TEST_F(WalReplayE2E, RecordsSurviveWalDestruction) {
    {
        Wal wal1{walPath_, clock_};
        ASSERT_TRUE(wal1.append(kIncomingJson, "cid-a").has_value());
        ASSERT_TRUE(wal1.append(kIncomingJson, "cid-b").has_value());
        ASSERT_TRUE(wal1.append(kIncomingJson, "cid-c").has_value());
    } // ~Wal closes the fd

    Wal wal2{walPath_, clock_};
    auto pending = wal2.readAll();
    ASSERT_EQ(pending.size(), 3U);
    EXPECT_EQ(pending[0].correlationId, "cid-a");
    EXPECT_EQ(pending[1].correlationId, "cid-b");
    EXPECT_EQ(pending[2].correlationId, "cid-c");
}

// Replay path: pending records flow into Mailbox::enqueueReplay, the matching
// handler runs against the fake ports, and the WAL is truncated to empty
// once every handler completes successfully. This is the "replay is
// indistinguishable from live POST" contract.
TEST_F(WalReplayE2E, EnqueueReplayDispatchesToHandlersAndTruncates) {
    // Phase 1: write one Incoming record while no mailbox is running.
    {
        Wal wal1{walPath_, clock_};
        ASSERT_TRUE(wal1.append(kIncomingJson, "cid-r1").has_value());
    }

    // Phase 2: stand up a fresh Wal + Mailbox with the real Incoming handler.
    FakeTicketStore ts;
    FakeAddressBook ab;
    FakeUiNotifier ui;
    HandleIncomingCall incoming{ts, ab, ui, fakeClock_, cfg_};

    ab.canonicalizeMap["+491701234567"] = "+491701234567";
    Contact c;
    c.name = "Alice";
    c.projectIds = {ProjectId{"P1"}};
    ab.nextLookup.push_back(std::optional<Contact>{c});
    ts.nextFindOpenInProjectByCallerNumber.push_back(std::optional<aid::Ticket>{});
    ts.nextCreate.push_back(TicketId{"T-r1"});

    LoopThread lt;
    Wal wal2{walPath_, clock_};
    auto pending = wal2.readAll();
    ASSERT_EQ(pending.size(), 1U);

    Mailbox::Handlers handlers;
    handlers.incoming = [&](const IncomingCall& ev, bool replay) -> Task<Result<void>> {
        co_return co_await incoming.run(ev, replay);
    };
    Mailbox mb{lt.loop(), wal2, Logger::instance(), std::move(handlers),
               &CallController::decodeJson};

    for (auto& rec : pending) {
        mb.enqueueReplay(rec);
    }

    ASSERT_TRUE(waitUntil([&] { return ts.created.size() == 1; }))
        << "replay must drive HandleIncomingCall::run";
    EXPECT_EQ(ts.created[0].callId, CallId{"replay-1"});
    EXPECT_EQ(ts.created[0].projectId, ProjectId{"P1"});

    ASSERT_TRUE(waitUntil([&] {
        std::ifstream f(walPath_);
        std::string contents((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        return contents.empty();
    })) << "WAL must be empty after a successful replay";
}

// Constructing a Mailbox without a decoder is the documented "main wasn't
// wired correctly" failure mode. enqueueReplay must log + drop the record
// rather than crash, and the WAL stays intact so an operator can replay
// after fixing the bootstrap.
TEST_F(WalReplayE2E, EnqueueReplayWithoutDecoder_DropsRecordWithoutCrash) {
    {
        Wal wal1{walPath_, clock_};
        ASSERT_TRUE(wal1.append(kIncomingJson, "cid-x").has_value());
    }

    FakeTicketStore ts;
    FakeAddressBook ab;
    FakeUiNotifier ui;
    LoopThread lt;
    Wal wal2{walPath_, clock_};
    auto pending = wal2.readAll();
    ASSERT_EQ(pending.size(), 1U);

    Mailbox mb{lt.loop(), wal2, Logger::instance(), Mailbox::Handlers{}, nullptr};

    EXPECT_NO_THROW(mb.enqueueReplay(pending[0]));

    // No handlers were touched; the WAL still holds the original line.
    EXPECT_EQ(ts.created.size(), 0U);
    EXPECT_EQ(ui.invalidateScopes.size(), 0U);
    std::ifstream f(walPath_);
    const std::string contents((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
    EXPECT_FALSE(contents.empty());
}
