// aid-daemon entry point.
//
// Wires every layer of the daemon together — Config, Logger, plugins,
// auth, mailbox, controllers — and runs Drogon's event loop until SIGTERM
// triggers a graceful 10s mailbox drain. This is the only file that
// touches the global drogon::app() and the only file that knows about
// every class.

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <drogon/utils/coroutine.h>
#include <signal.h>
#include <sodium.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <trantor/net/EventLoop.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "aid/abi/PluginContract.h"
#include "aid/adapters/ws/WsHubAdapter.h"
#include "aid/auth/AuthDb.h"
#include "aid/auth/AuthService.h"
#include "aid/auth/PasswordHasher.h"
#include "aid/auth/ResetGrantStore.h"
#include "aid/auth/SessionRepo.h"
#include "aid/auth/UserGate.h"
#include "aid/auth/UserRepo.h"
#include "aid/controllers/CallController.h"
#include "aid/controllers/HealthController.h"
#include "aid/controllers/LoginController.h"
#include "aid/controllers/SessionGuard.h"
#include "aid/controllers/UiController.h"
#include "aid/controllers/UiStreamController.h"
#include "aid/controllers/WebhookController.h"
#include "aid/crosscutting/Clock.h"
#include "aid/crosscutting/Config.h"
#include "aid/crosscutting/CorrelationId.h"
#include "aid/crosscutting/Logger.h"
#include "aid/infrastructure/HealthService.h"
#include "aid/infrastructure/Mailbox.h"
#include "aid/infrastructure/MembershipReconciler.h"
#include "aid/infrastructure/PluginLoader.h"
#include "aid/infrastructure/StartupSequencer.h"
#include "aid/infrastructure/Wal.h"
#include "aid/infrastructure/WebhookMailbox.h"
#include "aid/plumbing/Error.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/ports/AddressBook.h"
#include "aid/ports/TicketStore.h"
#include "aid/usecases/AppendComment.h"
#include "aid/usecases/CloseTicket.h"
#include "aid/usecases/GetDashboard.h"
#include "aid/usecases/HandleAcceptedCall.h"
#include "aid/usecases/HandleHangup.h"
#include "aid/usecases/HandleIncomingCall.h"
#include "aid/usecases/HandleOutgoingCall.h"
#include "aid/usecases/HandleTransferCall.h"
#include "aid/usecases/ReconcileMemberships.h"
#include "aid/usecases/TicketDeltaEmitter.h"
#include "aid/value-types/CallEvent.h"
#include "aid/version.hpp"

namespace {

using aid::AcceptedCall;
using aid::HangupCall;
using aid::IncomingCall;
using aid::OutgoingCall;
using aid::TransferCall;
using aid::adapters::ws::WsHubAdapter;
using aid::auth::AuthDb;
using aid::auth::AuthService;
using aid::auth::PasswordHasher;
using aid::auth::ResetGrantStore;
using aid::auth::SessionRepo;
using aid::auth::UserRepo;
using aid::controllers::CallController;
using aid::controllers::HealthController;
using aid::controllers::LoginController;
using aid::controllers::SessionGuard;
using aid::controllers::UiController;
using aid::controllers::UiStreamController;
using aid::controllers::WebhookController;
using aid::crosscutting::Config;
using aid::crosscutting::CorrelationId;
using aid::crosscutting::Logger;
using aid::crosscutting::LogLevel;
using aid::crosscutting::RealClock;
using aid::infrastructure::checkPluginAbiLayoutTag;
using aid::infrastructure::checkPluginApiVersion;
using aid::infrastructure::checkPluginContractTag;
using aid::infrastructure::HealthService;
using aid::infrastructure::Mailbox;
using aid::infrastructure::MembershipReconciler;
using aid::infrastructure::PluginLoader;
using aid::infrastructure::Wal;
using aid::infrastructure::WebhookMailbox;
using aid::plumbing::Error;
using aid::plumbing::ErrorCode;
using aid::plumbing::Result;
using aid::plumbing::Task;
using aid::plumbing::unexpected;
using aid::ports::AddressBook;
using aid::ports::TicketStore;
using aid::usecases::AppendComment;
using aid::usecases::CloseTicket;
using aid::usecases::GetDashboard;
using aid::usecases::HandleAcceptedCall;
using aid::usecases::HandleHangup;
using aid::usecases::HandleIncomingCall;
using aid::usecases::HandleOutgoingCall;
using aid::usecases::HandleTransferCall;
using aid::usecases::ReconcileMemberships;
using aid::usecases::TicketDeltaEmitter;

// The inbox WAL path is config-driven (top-level "walPath", default
// /var/lib/aid-daemon/inbox.log — see Config::walPath). The Phase-6 webhook
// durability log is a SEPARATE WAL so a webhook
// body (re-keyed by ticket id on replay) never collides with the per-callid
// replay decoder; Main derives it as a sibling "webhook.log" in the SAME
// directory, so preflight()'s single WAL-dir writability check covers both.
constexpr const char* kWebhookWalFilename = "webhook.log";

LogLevel parseLogLevel(std::string_view s) {
    if (s == "TRACE")
        return LogLevel::TRACE;
    if (s == "DEBUG")
        return LogLevel::DEBUG;
    if (s == "INFO")
        return LogLevel::INFO;
    if (s == "WARN")
        return LogLevel::WARN;
    if (s == "ERROR")
        return LogLevel::ERROR;
    if (s == "FATAL")
        return LogLevel::FATAL;
    return LogLevel::INFO;
}

bool isWritableDir(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return false;
    return ::access(dir.c_str(), W_OK | X_OK) == 0;
}

// Each failure returns a single
// short Error suitable for fatal-log-and-exit. config-file mode is
// already enforced inside Config::load; we re-check the dependent
// filesystem state here so the operator gets one clear failure before
// dlopen / sqlite open / port bind.
Result<void> preflight(const Config& cfg) {
    namespace fs = std::filesystem;

    // (a) Logger paths — parent dirs must be writable.
    auto loggerCfg = cfg.logger();
    if (!loggerCfg)
        return unexpected(loggerCfg.error());
    for (const auto& p : {loggerCfg->backendLogPath, loggerCfg->frontendLogPath}) {
        const auto parent = fs::path(p).parent_path();
        if (!parent.empty() && !isWritableDir(parent)) {
            return unexpected(Error{ErrorCode::InvalidInput,
                                    "preflight: log directory not writable: " + parent.string(),
                                    std::nullopt});
        }
    }

    // (b) WAL directory writable. Path is config-driven (top-level "walPath");
    // the webhook WAL is a sibling in the same directory, so this covers both.
    auto walPathR = cfg.walPath();
    if (!walPathR)
        return unexpected(walPathR.error());
    const auto walDir = walPathR->parent_path();
    if (!isWritableDir(walDir)) {
        return unexpected(Error{ErrorCode::InvalidInput,
                                "preflight: WAL directory not writable: " + walDir.string(),
                                std::nullopt});
    }

    // (c) Auth DB directory writable.
    auto authCfg = cfg.auth();
    if (!authCfg)
        return unexpected(authCfg.error());
    const auto authDir = authCfg->dbPath.parent_path();
    if (!authDir.empty() && !isWritableDir(authDir)) {
        return unexpected(Error{ErrorCode::InvalidInput,
                                "preflight: auth DB directory not writable: " + authDir.string(),
                                std::nullopt});
    }

    // (d) Plugin .so paths exist and are not world-writable.
    //     PluginLoader::load re-checks ownership/group exactly; this is the
    //     fail-fast pre-check so the operator sees a config-shaped error
    //     before we even try to dlopen.
    auto plugins = cfg.plugins();
    if (!plugins)
        return unexpected(plugins.error());
    for (const auto& p : {plugins->ticketStoreSoPath, plugins->addressBookSoPath}) {
        struct stat st {};
        if (::stat(p.c_str(), &st) != 0) {
            return unexpected(Error{ErrorCode::InvalidInput, "preflight: plugin .so missing: " + p,
                                    std::nullopt});
        }
        if ((st.st_mode & S_IWOTH) != 0) {
            return unexpected(Error{ErrorCode::InvalidInput,
                                    "preflight: plugin .so is world-writable: " + p, std::nullopt});
        }
    }

    // (e) UI document root (optional). Must be an existing directory containing
    //     index.html and not world-writable — same fail-fast hygiene as the
    //     plugin .so check above, so a typo'd Ui.documentRoot fails as a config
    //     error before any listener opens. NOTE: unlike the config file
    //     (O_NOFOLLOW), a symlinked docroot is allowed — symlinked release dirs
    //     are a normal deploy pattern.
    auto uiCfg = cfg.ui();
    if (!uiCfg)
        return unexpected(uiCfg.error());
    if (uiCfg->documentRoot) {
        const auto& root = *uiCfg->documentRoot;
        std::error_code ec;
        if (!fs::is_directory(root, ec)) {
            return unexpected(Error{
                ErrorCode::InvalidInput,
                "preflight: Ui.documentRoot is not a directory: " + root.string(), std::nullopt});
        }
        struct stat st {};
        if (::stat(root.c_str(), &st) == 0 && (st.st_mode & S_IWOTH) != 0) {
            return unexpected(Error{
                ErrorCode::InvalidInput,
                "preflight: Ui.documentRoot is world-writable: " + root.string(), std::nullopt});
        }
        if (!fs::is_regular_file(root / "index.html", ec)) {
            return unexpected(Error{ErrorCode::InvalidInput,
                                    "preflight: Ui.documentRoot has no index.html (did you run "
                                    "scripts/build-ui.sh?): " +
                                        root.string(),
                                    std::nullopt});
        }
    }

    // TLS files are not configured in v1 (HTTP-only on the LAN / VPN).
    return {};
}

// Dedicated domain loop — hosts mailbox workers + plugin
// HttpClients. Runs on a side thread for the lifetime of the daemon.
// drogon::app().getIOLoop(0) returns null before app().run(), so we own
// the loop ourselves and hand the pointer to plugins/Mailbox at load
// time. RAII: destructor quits the loop and joins the thread.
class DomainLoop {
public:
    DomainLoop() {
        std::promise<trantor::EventLoop*> ready;
        auto fut = ready.get_future();
        thread_ = std::thread([&ready]() {
            trantor::EventLoop loop;
            ready.set_value(&loop);
            loop.loop();
        });
        loop_ = fut.get();
    }
    DomainLoop(const DomainLoop&) = delete;
    DomainLoop& operator=(const DomainLoop&) = delete;
    DomainLoop(DomainLoop&&) = delete;
    DomainLoop& operator=(DomainLoop&&) = delete;
    ~DomainLoop() { stop(); }

    void stop() noexcept {
        if (stopped_.exchange(true))
            return;
        if (loop_ != nullptr) {
            loop_->queueInLoop([l = loop_]() { l->quit(); });
        }
        if (thread_.joinable())
            thread_.join();
    }

    [[nodiscard]] trantor::EventLoop* get() const noexcept { return loop_; }

private:
    std::thread thread_;
    trantor::EventLoop* loop_{nullptr};
    std::atomic<bool> stopped_{false};
};

// Atomic shutdown flag set from the SIGTERM/SIGINT signal handler. We
// only mutate this from the handler (an atomic<bool>::store is
// async-signal-safe on the platforms we target) and observe it from a
// timer running on the main Drogon loop, which means the actual drain
// runs from a non-signal context. The handler itself does nothing else.
std::atomic<bool> g_shutdownRequested{false};

extern "C" void onSignal(int /*sig*/) noexcept {
    g_shutdownRequested.store(true, std::memory_order_release);
}

} // namespace

int main(int argc, char** argv) {
    // -------- 0. Restrict file-creation mask BEFORE anything writes. --------
    // SQLite creates auth.db-wal and auth.db-shm lazily on the first write,
    // inheriting the process umask. 0077 = owner-only on every file this
    // process creates — protects in-flight password hashes from group / world.
    ::umask(0077);

    // -------- 1. argv + Config + Logger --------
    if (argc < 2) {
        std::cerr << "usage: aid-daemon <config.json>" << std::endl;
        return 1;
    }
    auto cfg = Config::load(argv[1]);
    if (!cfg) {
        std::cerr << "config: " << cfg.error().message << std::endl;
        return 1;
    }

    auto loggerCfg = cfg->logger();
    if (!loggerCfg) {
        std::cerr << "config.logger: " << loggerCfg.error().message << std::endl;
        return 1;
    }
    try {
        Logger::initialize(parseLogLevel(loggerCfg->level), loggerCfg->backendLogPath,
                           loggerCfg->frontendLogPath);
    } catch (const std::exception& e) {
        std::cerr << "logger: " << e.what() << std::endl;
        return 1;
    }
    Logger::routeTrantor();
    Logger::instance().info(std::string{"aid-daemon "} + std::string{aid::version()} + " starting");

    // -------- 2. Pre-flight. --------
    if (auto p = preflight(*cfg); !p) {
        Logger::instance().fatal(p.error().message);
        return 1;
    }

    // -------- 3. libsodium one-shot — BEFORE AuthDb opens or Argon2 verify runs. --------
    if (::sodium_init() < 0) {
        Logger::instance().fatal("sodium_init failed");
        return 1;
    }

    // -------- 4. Plugin load (load once, never dlclose). --------
    auto pluginsCfg = cfg->plugins();
    if (!pluginsCfg) {
        Logger::instance().fatal(pluginsCfg.error().message);
        return 1;
    }
    const auto opSection = cfg->sectionJson("TicketSystem");
    if (!opSection || !opSection->has_value()) {
        Logger::instance().fatal(
            "config: TicketSystem section missing (plugin would have no config)");
        return 1;
    }
    const auto abSection = cfg->sectionJson("AddressSystem");
    if (!abSection || !abSection->has_value()) {
        Logger::instance().fatal("config: AddressSystem section missing");
        return 1;
    }

    PluginLoader<TicketStore> ticketStorePlugin;
    PluginLoader<AddressBook> addressBookPlugin;

    // PluginLoader checks owner-uid == expectedOwnerUid; in production this is
    // 0 (root-owned plugins under /usr/lib). On the developer box the plugins
    // are owned by the build user, so pass ::geteuid() to match. The
    // expectedOwnerUid default is 0 for prod.
    const ::uid_t expectedOwner = ::geteuid();

    // The dedicated domain loop: a side-thread trantor::EventLoop
    // that hosts the plugin HttpClients and every mailbox worker.
    // drogon::app().getIOLoop(0) is null before .run(), so we own the loop
    // ourselves and tear it down explicitly on shutdown (before plugin
    // destructors run so no callback can resume after the loop quits).
    DomainLoop domainLoop;

    if (auto r = ticketStorePlugin.loadWithLoop(pluginsCfg->ticketStoreSoPath, "create_TicketStore",
                                                "destroy_TicketStore", **opSection,
                                                domainLoop.get(), expectedOwner);
        !r) {
        Logger::instance().fatal(r.error().message);
        return 1;
    }
    // ABI handshake: refuse to start on an incompatible plugin version.
    if (auto v = checkPluginApiVersion(ticketStorePlugin.apiVersion(), "TicketStore"); !v) {
        Logger::instance().fatal(v.error().message);
        return 1;
    }
    // ABI layout guard: refuse to start on a value-type layout skew (a
    // boundary struct changed but this plugin wasn't rebuilt) before it can
    // corrupt the heap on the dashboard or teardown path.
    if (auto v = checkPluginAbiLayoutTag(ticketStorePlugin.abiLayoutTag(), "TicketStore"); !v) {
        Logger::instance().fatal(v.error().message);
        return 1;
    }
    // Stale-plugin guard: refuse to start on a plugin whose behaviour
    // contract lags the daemon (or predates the tag entirely) — that's the
    // silent "loaded cleanly but did nothing new" trap. checkPluginContractTag
    // treats an absent tag as a hard failure, same as the layout guard.
    if (auto v = checkPluginContractTag(ticketStorePlugin.contractTag(), "TicketStore"); !v) {
        Logger::instance().fatal(v.error().message);
        return 1;
    }
    if (auto r = addressBookPlugin.loadWithLoop(pluginsCfg->addressBookSoPath, "create_AddressBook",
                                                "destroy_AddressBook", **abSection,
                                                domainLoop.get(), expectedOwner);
        !r) {
        Logger::instance().fatal(r.error().message);
        return 1;
    }
    if (auto v = checkPluginApiVersion(addressBookPlugin.apiVersion(), "AddressBook"); !v) {
        Logger::instance().fatal(v.error().message);
        return 1;
    }
    if (auto v = checkPluginAbiLayoutTag(addressBookPlugin.abiLayoutTag(), "AddressBook"); !v) {
        Logger::instance().fatal(v.error().message);
        return 1;
    }
    if (auto v = checkPluginContractTag(addressBookPlugin.contractTag(), "AddressBook"); !v) {
        Logger::instance().fatal(v.error().message);
        return 1;
    }
    // Record the contract level that passed for both plugins. Embedding the
    // daemon's own kPluginContractTag in this log line also bakes the greppable
    // token into the daemon binary, which scripts/deploy.sh reads back as the
    // expected value when verifying each just-copied .so.
    Logger::instance().info(std::string{"plugin behaviour contract OK: "} +
                            aid::abi::kPluginContractTag + " (TicketStore + AddressBook)");

    // -------- 5. In-process adapters + cross-cutting infra. --------
    RealClock clock;
    CorrelationId cid;
    // Config-driven WAL path (preflight already validated its directory). The
    // webhook WAL is its sibling "webhook.log" in the same directory.
    auto walPathR = cfg->walPath();
    if (!walPathR) {
        Logger::instance().fatal(walPathR.error().message);
        return 1;
    }
    const std::filesystem::path walPath = *walPathR;
    const std::filesystem::path webhookWalPath = walPath.parent_path() / kWebhookWalFilename;
    Wal wal{walPath.string(), clock};
    WsHubAdapter wsHub{Logger::instance()};

    // -------- 6. Auth (AuthDb::open enforces mode 0600 if file exists). --------
    auto authCfg = cfg->auth();
    if (!authCfg) {
        Logger::instance().fatal(authCfg.error().message);
        return 1;
    }
    auto authDb = AuthDb::open(authCfg->dbPath);
    if (!authDb) {
        Logger::instance().fatal(authDb.error().message);
        return 1;
    }
    UserRepo userRepo{*authDb, clock};
    SessionRepo sessionRepo{*authDb, clock, *authCfg};
    AuthService authService{userRepo, sessionRepo, clock, *authCfg};
    // Single-use password-reset grants (recovery-key flow). Default 5-min
    // TTL; lifetime matches authService — both live until app().run() returns.
    ResetGrantStore resetGrants{clock};
    // Warm the dummy Argon2 hash on the bootstrap thread so the first
    // unknown-username login does not pay the 50–200 ms compute cost on
    // its critical path — preserves the timing-equal property end-to-end.
    (void)PasswordHasher::dummyHash();

    // Dedicated auth.db connection for the inbound-call membership gate
    // (aid::auth::userKnown, wired into the mailbox handlers below). This gate
    // runs on the DOMAIN loop, whereas userRepo/sessionRepo above are confined
    // to Drogon's single IO loop (SessionGuard/LoginController). AuthDb has no
    // internal mutex and caches prepared statements PER CONNECTION, so the two
    // threads must not share one connection — that would race stmtCache_ and the
    // shared sqlite3_stmt handles. A second connection keeps each connection
    // single-thread-confined; auth.db's WAL mode + busy_timeout=5000 (set by
    // AuthDb::open) make cross-connection reads consistent and non-BUSY.
    auto authDbGate = AuthDb::open(authCfg->dbPath);
    if (!authDbGate) {
        Logger::instance().fatal(authDbGate.error().message);
        return 1;
    }
    UserRepo userRepoGate{*authDbGate, clock};

    // -------- 7. Use cases (constructor DI — no service container). --------
    auto routing = cfg->ticketRouting();
    if (!routing) {
        Logger::instance().fatal(routing.error().message);
        return 1;
    }

    HandleIncomingCall incoming{*ticketStorePlugin.get(), *addressBookPlugin.get(), wsHub, clock,
                                *routing};
    HandleOutgoingCall outgoing{*ticketStorePlugin.get(), *addressBookPlugin.get(), wsHub, clock,
                                *routing};
    HandleAcceptedCall accepted{*ticketStorePlugin.get(), wsHub, clock};
    HandleTransferCall transfer{*ticketStorePlugin.get(), wsHub};
    HandleHangup hangup{*ticketStorePlugin.get(), wsHub, clock};
    GetDashboard dashboard{*ticketStorePlugin.get(), *addressBookPlugin.get()};
    AppendComment comment{*ticketStorePlugin.get(), wsHub};
    CloseTicket closeTk{*ticketStorePlugin.get(), wsHub};

    // Mailbox handlers — lambdas around each use case (canonical pattern
    // from tests/integration/it_call_dispatch.cpp).
    Mailbox::Handlers handlers;
    handlers.incoming = [&incoming](const IncomingCall& ev, bool replay) -> Task<Result<void>> {
        co_return co_await incoming.run(ev, replay);
    };
    // The three user-bearing events (Outgoing/Transfer required, Accepted
    // optional) are gated on auth.db membership: an unknown handle drops the
    // event silently (co_return Result<void>{} acks the WAL — no retry). This
    // runs on the domain loop before the use case; see aid::auth::userKnown.
    // Uses userRepoGate (the domain-loop-owned auth.db connection), never the
    // IO-loop userRepo — see the earlier connection comment.
    handlers.outgoing = [&outgoing, &userRepoGate](const OutgoingCall& ev,
                                                   bool replay) -> Task<Result<void>> {
        if (!aid::auth::userKnown(userRepoGate, ev.user)) {
            co_return Result<void>{};
        }
        co_return co_await outgoing.run(ev, replay);
    };
    handlers.accepted = [&accepted, &userRepoGate](const AcceptedCall& ev) -> Task<Result<void>> {
        if (!aid::auth::userKnown(userRepoGate, ev.user)) { // ev.user is already optional
            co_return Result<void>{};
        }
        co_return co_await accepted.run(ev);
    };
    handlers.transfer = [&transfer, &userRepoGate](const TransferCall& ev) -> Task<Result<void>> {
        if (!aid::auth::userKnown(userRepoGate, ev.newUser)) {
            co_return Result<void>{};
        }
        co_return co_await transfer.run(ev);
    };
    handlers.hangup = [&hangup](const HangupCall& ev) -> Task<Result<void>> {
        co_return co_await hangup.run(ev);
    };
    Mailbox mailbox{*domainLoop.get(), wal, Logger::instance(), std::move(handlers),
                    &CallController::decodeJson};

    // -------- 8. WAL replay BEFORE listeners open (at-least-once). --------
    // enqueueReplay posts work onto the domain loop; the loop isn't spinning
    // yet, so the records sit in its task queue. Listeners are not yet
    // attached either, so the at-least-once invariant holds: replay runs
    // before any new /call POST is accepted.
    aid::infrastructure::StartupSequencer startupSeq;
    for (auto& rec : wal.readAll()) {
        mailbox.enqueueReplay(rec);
    }
    startupSeq.markReplayComplete();

    // -------- 8b. Webhook ingest (Phase 6, optional). --------
    // Reflect edits made directly in the ticket-system UI: a per-ticket mailbox
    // mirroring the per-callid one above. Only stood up when a [Webhook] config
    // section is present; otherwise inbound webhooks are simply not accepted.
    auto webhookCfg = cfg->webhook();
    if (!webhookCfg) {
        Logger::instance().fatal(webhookCfg.error().message);
        return 1;
    }
    std::optional<Wal> webhookWal;
    std::optional<WebhookMailbox> webhookMailbox;
    std::shared_ptr<WebhookController> webhookCtl;
    if (webhookCfg->has_value()) {
        webhookWal.emplace(webhookWalPath.string(), clock);

        // Worker body: decode the webhook → on a genuine (non-echo) ticket, emit
        // the SAME per-recipient live delta the call flow emits. decodeWebhook
        // returns nullopt for the daemon's own echoes (suppressed) so a dashboard
        // edit updates exactly once.
        TicketStore& ts = *ticketStorePlugin.get();
        WebhookMailbox::Handler handler = [&ts, &wsHub](std::string payload,
                                                        std::string /*cid*/) -> Task<Result<void>> {
            auto decoded = co_await ts.decodeWebhook(std::move(payload));
            if (!decoded) {
                co_return unexpected(decoded.error());
            }
            if (!decoded->has_value()) {
                co_return Result<void>{}; // self-induced echo — nothing to push.
            }
            auto& wd = **decoded;
            // Log here, in core, the boundary where a webhook becomes live UI:
            // an admin removing a handler (customField7) in the ticket-system UI
            // drops the ticket off any recipient who was visible ONLY via that
            // handler entry. emitWebhookDelta upserts the ticket's current
            // recipients and then pushes each dropped login a ticket_remove.
            for (const auto& login : wd.droppedRecipients) {
                Logger::instance().info("webhook: ticket " + wd.ticket.id.v +
                                        " handler-drop -> live ticket_remove to " + login.v);
            }
            TicketDeltaEmitter emitter{ts, wsHub};
            co_return co_await emitter.emitWebhookDelta(std::move(wd));
        };
        webhookMailbox.emplace(*domainLoop.get(), *webhookWal, Logger::instance(),
                               std::move(handler), &WebhookController::ticketIdOf);

        for (auto& rec : webhookWal->readAll()) {
            webhookMailbox->enqueueReplay(rec);
        }

        webhookCtl = std::make_shared<WebhookController>(
            *webhookWal, *webhookMailbox, Logger::instance(), cid, (*webhookCfg)->secret);
        Logger::instance().info("webhook ingest enabled (POST /hook/ticket)");
    }

    // -------- 9. Cold-start health ping — /health meaningful from second 1. --------
    HealthService health{*ticketStorePlugin.get(), *addressBookPlugin.get(), mailbox,
                         /*pluginsLoaded=*/true};
    // Drive the ping synchronously on this thread, but dispatch the
    // coroutine onto the domain loop so every co_await internal to
    // bootstrapPing() resumes on the same loop the plugin captured. This
    // avoids the cross-loop coroutine hazard
    // (a coroutine started on loop A and resumed on loop B is UB). We
    // poll an atomic<bool> set from inside the dispatched coroutine —
    // simple and race-free because the only cross-thread communication
    // is the atomic, properly ordered with acquire/release.
    {
        std::atomic<bool> pingDone{false};
        domainLoop.get()->queueInLoop([&health, &pingDone]() {
            // drogon::AsyncTask self-destroys on completion (suspend_never on
            // both initial and final suspend) — no manual lifetime mgmt needed.
            [](HealthService& h, std::atomic<bool>& done) -> drogon::AsyncTask {
                try {
                    co_await h.bootstrapPing();
                } catch (const std::exception& e) {
                    Logger::instance().error(std::string{"bootstrap ping threw: "} + e.what());
                } catch (...) {
                    Logger::instance().error("bootstrap ping threw unknown");
                }
                done.store(true, std::memory_order_release);
                co_return;
            }(health, pingDone);
        });
        // bootstrapPing should return well under a second for a healthy upstream;
        // an unreachable upstream times out per the plugin's HttpClient settings
        // and still resolves with status="unreachable" cached in the Snapshot.
        while (!pingDone.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
    }

    // -------- 10. Register controllers + filter + listeners. --------
    UiStreamController::install(wsHub, Logger::instance(), cid);

    auto callCtl = std::make_shared<CallController>(wal, mailbox, Logger::instance(), cid);
    auto uiCtl =
        std::make_shared<UiController>(dashboard, comment, closeTk, cid, Logger::instance());
    auto healthCtl = std::make_shared<HealthController>(health);
    auto loginCtl = std::make_shared<LoginController>(authService, resetGrants, Logger::instance(),
                                                      cid, *authCfg);

    // SessionGuard is registered once globally; per-route gating happens by
    // listing the filter class name in each handler's constraint list.
    auto sessionGuard =
        std::make_shared<SessionGuard>(userRepo, sessionRepo, clock, Logger::instance(), *authCfg);
    drogon::app().registerFilter(sessionGuard);

    using drogon::HttpRequestPtr;
    using drogon::HttpResponsePtr;
    using HttpCallback = std::function<void(const HttpResponsePtr&)>;

    // /call → CallController. Loopback-only (calls.py runs on the same host).
    drogon::app().registerHandler("/call",
                                  [callCtl](const HttpRequestPtr& req, HttpCallback&& cb) {
                                      callCtl->handlePost(req, std::move(cb));
                                  },
                                  {drogon::Post});

    // /hook/ticket → WebhookController (Phase 6). Registered ONLY when the
    // optional Webhook config section is present; the controller itself gates on
    // the shared secret, so no SessionGuard (the webhook is not a UI session).
    if (webhookCtl) {
        drogon::app().registerHandler("/hook/ticket",
                                      [webhookCtl](const HttpRequestPtr& req, HttpCallback&& cb) {
                                          webhookCtl->handlePost(req, std::move(cb));
                                      },
                                      {drogon::Post});
    }

    // /health → HealthController. No auth (trust-the-LAN, same as /call).
    drogon::app().registerHandler("/health",
                                  [healthCtl](const HttpRequestPtr& req, HttpCallback&& cb) {
                                      healthCtl->get(req, std::move(cb));
                                  },
                                  {drogon::Get});

    // /ui/login → LoginController, no SessionGuard (this is how you get a session).
    drogon::app().registerHandler("/ui/login",
                                  [loginCtl](const HttpRequestPtr& req, HttpCallback&& cb) {
                                      loginCtl->postLogin(req, std::move(cb));
                                  },
                                  {drogon::Post});

    // /ui/reset → LoginController, no SessionGuard. Gated instead by the
    // single-use `aid_reset` grant cookie minted on a recovery-key login;
    // SessionGuard would reject it (no session cookie), which is exactly
    // why it must not sit behind the filter.
    drogon::app().registerHandler("/ui/reset",
                                  [loginCtl](const HttpRequestPtr& req, HttpCallback&& cb) {
                                      loginCtl->postReset(req, std::move(cb));
                                  },
                                  {drogon::Post});

    // /ui/logout, /ui/whoami → behind SessionGuard.
    drogon::app().registerHandler("/ui/logout",
                                  [loginCtl](const HttpRequestPtr& req, HttpCallback&& cb) {
                                      loginCtl->postLogout(req, std::move(cb));
                                  },
                                  {drogon::Post, "aid::controllers::SessionGuard"});
    drogon::app().registerHandler("/ui/whoami",
                                  [loginCtl](const HttpRequestPtr& req, HttpCallback&& cb) {
                                      loginCtl->getWhoami(req, std::move(cb));
                                  },
                                  {drogon::Get, "aid::controllers::SessionGuard"});

    // /ui/dashboard, /ui/comment/{ticketId}, /ui/close/{ticketId} → behind SessionGuard.
    drogon::app().registerHandler("/ui/dashboard",
                                  [uiCtl](const HttpRequestPtr& req, HttpCallback&& cb) {
                                      uiCtl->getDashboard(req, std::move(cb));
                                  },
                                  {drogon::Get, "aid::controllers::SessionGuard"});
    drogon::app().registerHandler(
        "/ui/comment/{ticketId}",
        [uiCtl](const HttpRequestPtr& req, HttpCallback&& cb, std::string ticketId) {
            uiCtl->postComment(req, std::move(cb), std::move(ticketId));
        },
        {drogon::Post, "aid::controllers::SessionGuard"});
    drogon::app().registerHandler(
        "/ui/close/{ticketId}",
        [uiCtl](const HttpRequestPtr& req, HttpCallback&& cb, std::string ticketId) {
            uiCtl->postClose(req, std::move(cb), std::move(ticketId));
        },
        {drogon::Post, "aid::controllers::SessionGuard"});

    // /ui/stream — owned by UiStreamController via Drogon's WebSocketController
    // reflection (WS_PATH_ADD declares the SessionGuard filter inline).

    auto lan = cfg->lanInterface();
    if (!lan) {
        Logger::instance().fatal(lan.error().message);
        return 1;
    }
    auto listenPort = cfg->listenPort();
    if (!listenPort) {
        Logger::instance().fatal(listenPort.error().message);
        return 1;
    }

    // cookieSecure=false is a legitimate plain-HTTP loopback dev
    // setting, but combined with a non-loopback LAN bind it ships the HttpOnly
    // aid_session cookie in cleartext over the LAN (session hijack on a shared
    // network). Warn loudly rather than refuse to start: 0.0.0.0 is a
    // documented bind-everywhere dev case (Config.h) that must still come up,
    // and this is pure additive hardening. authCfg is in scope from earlier.
    if (aid::crosscutting::sessionCookieExposedOnLan(authCfg->cookieSecure, *lan)) {
        Logger::instance().warn(
            "SECURITY: Auth.cookieSecure=false while lanInterface=\"" + *lan +
            "\" is not loopback — the aid_session cookie will be sent in cleartext over "
            "the LAN (session-hijack risk on a shared network). Set Auth.cookieSecure=true "
            "and serve the dashboard over HTTPS, or bind lanInterface to loopback "
            "(127.0.0.1) for plain-HTTP development.");
    }

    // Enforce the ordering contract — WAL replay must have completed
    // before any listener opens. A future reorder that moves the replay loop
    // after this point trips here and aborts startup instead of silently
    // breaking at-least-once ordering.
    if (auto ordered = startupSeq.requireReplayedBeforeListening(); !ordered) {
        Logger::instance().fatal(ordered.error().message);
        return 1;
    }

    // listenPort() guarantees [1, 65535], so the narrowing to the
    // addListener port type is lossless.
    const auto port = static_cast<std::uint16_t>(*listenPort);
    drogon::app()
        .addListener("127.0.0.1", port)
        .addListener(*lan, port)
        .setClientMaxBodySize(1 * 1024 * 1024)
        .setClientMaxWebSocketMessageSize(1 * 1024 * 1024);

    // -------- 10b. SPA static serving (optional, gated on Ui.documentRoot). --------
    // Ship the built SvelteKit dashboard from this single daemon: same origin as
    // /ui/* and /ui/stream, so the HttpOnly session cookie and the WebSocket
    // "just work" with no CORS and no Node in prod. preflight() already validated
    // the directory + index.html; absent key => API-only (dev uses the Vite proxy).
    if (auto uiCfg = cfg->ui(); uiCfg && uiCfg->documentRoot) {
        const std::string docRoot = uiCfg->documentRoot->string();
        drogon::app().setDocumentRoot(docRoot);
        // Hashed _app/ assets are immutable, but index.html must never be
        // stale-cached or a deploy would serve old JS; 0 = no static cache header.
        drogon::app().setStaticFilesCacheTime(0);

        // setDefaultHandler runs ONLY after the registered handlers (/ui/*,
        // /health, /call) and the static-file router both miss — so it cannot
        // shadow them. Serve index.html for unknown GET routes that are not
        // API/event paths, so reloading a client-router deep link like /login
        // renders the SPA instead of 404.
        drogon::app().setDefaultHandler([docRoot](const HttpRequestPtr& req, HttpCallback&& cb) {
            const std::string& path = req->path();
            const bool isApi = path == "/health" || path == "/call" || path == "/ui" ||
                               path.rfind("/ui/", 0) == 0 || path.rfind("/hook/", 0) == 0;
            if (req->method() != drogon::Get || isApi) {
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k404NotFound);
                cb(resp);
                return;
            }
            cb(drogon::HttpResponse::newFileResponse(docRoot + "/index.html"));
        });
    }

    // -------- Membership reconciler. --------
    // The ticket system emits no membership webhook, so a long-lived daemon must POLL
    // to notice an admin adding/removing a project member (verified live).
    // Constructed ONLY when membershipPollIntervalSec > 0 (0 disables it). The
    // recurring timer runs on the domain loop that owns the plugin's
    // membersCache_ + HttpClient (loop affinity), is gated on ≥1
    // connected dashboard, and fires an immediate reconcile on the 0→1 WebSocket
    // transition (connect-kick). Declared here (top-level main scope) so it
    // outlives app().run(); explicitly stopped in the teardown tail BEFORE the
    // plugin instance is released, since an in-flight tick may be inside
    // ts.refreshMembership() on the domain loop.
    auto pollIntervalCfg = cfg->membershipPollIntervalSec();
    if (!pollIntervalCfg) {
        Logger::instance().fatal(pollIntervalCfg.error().message);
        return 1;
    }
    std::optional<ReconcileMemberships> reconcileMemberships;
    std::optional<MembershipReconciler> membershipReconciler;
    if (*pollIntervalCfg > 0) {
        TicketStore& ts = *ticketStorePlugin.get();
        // The diff→push use case lives as long as the reconciler that wraps it.
        // optional storage is stable, so the ApplyDeltas closure can hold a
        // reference to the contained object.
        reconcileMemberships.emplace(ts, wsHub);
        MembershipReconciler::ApplyDeltas applyDeltas =
            [&rm = *reconcileMemberships](
                std::vector<aid::MembershipDelta> deltas) -> Task<Result<void>> {
            co_return co_await rm.reconcile(std::move(deltas));
        };
        membershipReconciler.emplace(
            *domainLoop.get(), ts, [&wsHub]() { return wsHub.anyConnected(); },
            std::move(applyDeltas), Logger::instance(), std::chrono::seconds{*pollIntervalCfg});
        // Connect-kick: a freshly-arrived dashboard gets an immediate reconcile
        // instead of waiting up to a full poll interval.
        wsHub.setOnFirstConnect([&mr = *membershipReconciler]() { mr.kick(); });
        membershipReconciler->start();
        Logger::instance().info("membership reconciler enabled (poll every " +
                                std::to_string(*pollIntervalCfg) + "s)");
    } else {
        Logger::instance().info("membership reconciler disabled (membershipPollIntervalSec=0)");
    }

    // -------- 11. Hourly session prune. --------
    drogon::app().getLoop()->runEvery(3600.0, [&sessionRepo]() {
        if (auto r = sessionRepo.prune(); r && *r > 0) {
            Logger::instance().debug("pruned " + std::to_string(*r) + " expired sessions");
        }
    });

    // -------- 11b. Mailbox idle GC (1-min timer, 1 h idle cutoff). --------
    // gcIdleOlderThan locks mtx_, so it is safe from this loop while workers run
    // on the domain loop. Registered on the Drogon main loop — like the prune and
    // drain timers above — so it stops when app().run() returns, before the stack
    // unwind destroys `mailbox`; no firing can outlive the mailbox.
    drogon::app().getLoop()->runEvery(
        60.0, [&mailbox]() { mailbox.gcIdleOlderThan(std::chrono::hours{1}); });
    if (webhookMailbox) {
        drogon::app().getLoop()->runEvery(
            60.0, [&webhookMailbox]() { webhookMailbox->gcIdleOlderThan(std::chrono::hours{1}); });
    }

    // -------- 12. SIGTERM / SIGINT → 10s drain → quit. --------
    // The signal handler only sets an atomic flag (async-signal-safe). A loop
    // timer observes the flag and runs the actual drain from a fresh worker
    // thread — drain() must be called from a non-loop thread per Mailbox.h.
    struct sigaction sa {};
    sa.sa_handler = &onSignal;
    sa.sa_flags = SA_RESTART;
    ::sigemptyset(&sa.sa_mask);
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGINT, &sa, nullptr);

    std::atomic<bool> drainStarted{false};
    // The drain runs on its own thread (drain() MUST be off the loop, per
    // Mailbox.h) but is kept JOINABLE — not detached — so main can join it
    // after run() returns, before the stack unwind destroys `mailbox`. A
    // detached thread could still be inside drain()'s frame (referencing
    // &mailbox) at that point: a use-after-scope window. The
    // compare_exchange below guarantees the assignment happens at most once,
    // on the main-loop thread, so there is no race with the join.
    std::thread drainThread;
    drogon::app().getLoop()->runEvery(0.5, [&mailbox, &webhookMailbox, &ticketStorePlugin,
                                            &addressBookPlugin, &drainThread, &drainStarted]() {
        if (!g_shutdownRequested.load(std::memory_order_acquire))
            return;
        bool expected = false;
        if (!drainStarted.compare_exchange_strong(expected, true))
            return;
        Logger::instance().info("SIGTERM — draining…");
        // drain() MUST run on a non-loop thread per Mailbox.h:110-115. The
        // body is std::this_thread::sleep_for-based and has no co_awaits, so
        // the Task<void> runs eagerly to completion on this thread (no
        // suspension), then the destructor cleans up its empty frame.
        drainThread =
            std::thread{[&mailbox, &webhookMailbox, &ticketStorePlugin, &addressBookPlugin]() {
                try {
                    auto t = mailbox.drain(std::chrono::seconds{10});
                    (void)t;
                    if (webhookMailbox) {
                        // Same eager-run reasoning as the call mailbox above:
                        // drain() has no real co_await (sleep_for + co_return),
                        // so the Task runs to completion on this thread and the
                        // discard is safe.
                        auto wt = webhookMailbox->drain(std::chrono::seconds{10});
                        (void)wt;
                    }
                    // Any worker still active after the
                    // graceful budget is stuck in an upstream co_await (a hung
                    // backend). Cancel both plugins' in-flight requests WHILE
                    // the plugins are still alive — they are released only
                    // after this thread joins (main's tail) — so each suspended
                    // worker chain resumes with a terminal error and unwinds
                    // through LIVE plugin internals (resuming after release
                    // would run it through freed helpers → shutdown SIGSEGV).
                    if (auto* ts = ticketStorePlugin.get()) {
                        ts->cancelPendingRequests();
                    }
                    if (auto* ab = addressBookPlugin.get()) {
                        ab->cancelPendingRequests();
                    }
                    // Settle: the cancelled stragglers (and any worker in a
                    // sub-second retry backoff) now finish fast. Wait for both
                    // mailboxes to go quiescent before main's tail releases the
                    // plugins and unwinds the mailbox frames — so no frame is
                    // destroyed while still suspended in a plugin call.
                    {
                        auto t2 = mailbox.drain(std::chrono::seconds{5});
                        (void)t2;
                    }
                    if (webhookMailbox) {
                        auto wt2 = webhookMailbox->drain(std::chrono::seconds{5});
                        (void)wt2;
                    }
                } catch (const std::exception& e) {
                    Logger::instance().error(std::string{"drain threw: "} + e.what());
                }
                // Return from drogon::app().run() so main()'s tail can run the
                // ordered teardown. We deliberately do NOT stop the domain loop
                // here: the plugin destructors (run from main's tail) queue
                // HttpClient cleanup onto that loop, so it must outlive them.
                // Stopping the loop before the plugins are gone enqueues onto a
                // freed EventLoop (SIGSEGV in MpscQueue::enqueue).
                drogon::app().quit();
            }};
    });

    drogon::app().run();

    // Join the drain thread before any teardown: run() only returns after
    // the drain thread called quit(), so this join is short and closes the
    // use-after-scope window on &mailbox. Non-joinable (no shutdown drain) is
    // skipped. Must happen before `mailbox` unwinds at end of main.
    if (drainThread.joinable())
        drainThread.join();

    // Stop the membership reconciler FIRST — while both the domain loop and the
    // plugin instance are still alive. Its recurring timer fires on the domain
    // loop (which keeps running until domainLoop's destructor), and an in-flight
    // tick may be suspended inside ts.refreshMembership() on that loop; stop()
    // cancels the timer and drains the in-flight tick so nothing resumes on a
    // released plugin or a freed `this`. Idempotent — the optional's destructor
    // calls it again as a harmless no-op during unwind.
    if (membershipReconciler) {
        membershipReconciler->stop();
    }

    // Ordered teardown. Several objects queue cleanup onto the
    // domain EventLoop from their destructors (the plugins' HttpClients and
    // the Mailbox's worker map). They must all be destroyed while that loop
    // is still alive, otherwise the dtor enqueues onto a freed EventLoop
    // (SIGSEGV in MpscQueue::enqueue).
    //
    // The plugins are declared BEFORE `domainLoop`, so natural reverse-order
    // unwinding would destroy them AFTER the loop — release them explicitly
    // here, while the loop runs. Everything declared AFTER `domainLoop`
    // (mailbox, health, …) is destroyed before it during unwind, so it is
    // already safe; and `domainLoop`'s own RAII destructor stops the loop
    // LAST, draining the queued cleanup before joining. We deliberately do
    // NOT stop the loop here — doing so would strand the Mailbox destructor
    // (which still enqueues) on a dead loop.
    ticketStorePlugin.releaseInstance();
    addressBookPlugin.releaseInstance();

    Logger::instance().info("aid-daemon stopped");
    return 0;
}
