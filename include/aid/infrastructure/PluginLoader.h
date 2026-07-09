#pragma once

#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "aid/abi/PluginAbiTag.h"
#include "aid/abi/PluginContract.h"
#include "aid/plumbing/Error.h"
#include "aid/plumbing/Result.h"

// PluginLoader<Port>. Header-only template: every instantiation
// (TicketStore, AddressBook, …) compiles inline at its callsite. The
// loaded handle is held until process exit — we deliberately never
// dlclose, which eliminates the vtable-corruption class of bugs that
// plagued the old microkernel.

namespace aid::infrastructure {

template <class Port> class PluginLoader {
public:
    PluginLoader() = default;
    PluginLoader(const PluginLoader&) = delete;
    PluginLoader& operator=(const PluginLoader&) = delete;
    PluginLoader(PluginLoader&&) = delete;
    PluginLoader& operator=(PluginLoader&&) = delete;
    ~PluginLoader() = default;

    // expectedOwnerUid defaults to 0 (root) for production; tests pass
    // ::geteuid() so a non-root build user can host the .so. The world-
    // and group-writable checks are unconditional.
    [[nodiscard]] aid::plumbing::Result<void>
    load(std::string_view soPath, std::string_view createSym, std::string_view destroySym,
         std::string_view configJson, ::uid_t expectedOwnerUid = 0);

    // Loop-aware overload — for ports whose factory needs a
    // trantor::EventLoop* alongside config_json. The factory signature is
    //     extern "C" Port* create_<port>(const char* config_json, void* event_loop);
    // and event_loop is forwarded verbatim (void* keeps trantor types out
    // of the extern "C" boundary). The ticket-system plugin uses this overload to
    // receive the daemon's domain loop.
    [[nodiscard]] aid::plumbing::Result<void>
    loadWithLoop(std::string_view soPath, std::string_view createSym, std::string_view destroySym,
                 std::string_view configJson, void* eventLoop, ::uid_t expectedOwnerUid = 0);

    [[nodiscard]] Port* get() noexcept { return instance_.get(); }
    [[nodiscard]] const Port* get() const noexcept { return instance_.get(); }

    // Destroy the plugin instance now (invokes destroy_<Port> via the
    // FactoryDeleter) WITHOUT dlclose'ing the .so — the handle stays mapped
    // for the process lifetime per the plugin ABI.
    // Main calls this during ordered shutdown: a loop-aware plugin's
    // destructor queues HttpClient cleanup onto the shared domain EventLoop,
    // so the instance MUST be torn down while that loop is still alive.
    // Destroying it after the loop is gone enqueues onto a freed MpscQueue
    // (SIGSEGV). Idempotent — a second call is a no-op.
    void releaseInstance() noexcept { instance_.reset(); }

    // std::nullopt when the plugin omits aid_plugin_api_version() (per spec
    // the symbol is optional). PluginAbiMismatch only when no .so has been
    // loaded yet. Returning optional rather than a sentinel keeps "version 0"
    // and "no handshake" distinguishable, which Main needs for the ABI check.
    [[nodiscard]] aid::plumbing::Result<std::optional<int>> apiVersion() const;

    // The plugin's compiled-in value-type layout fingerprint, resolved
    // from the optional aid_plugin_abi_layout_tag() symbol. std::nullopt when
    // the symbol is absent (an older plugin) — checkPluginAbiLayoutTag treats
    // that as a hard failure since both shipped plugins always export it.
    // PluginAbiMismatch only when no .so has been loaded yet.
    [[nodiscard]] aid::plumbing::Result<std::optional<std::uint64_t>> abiLayoutTag() const;

    // The plugin's compiled-in behaviour-contract tag (BF3 stale-plugin guard),
    // resolved from the optional aid_plugin_contract_tag() symbol. std::nullopt
    // when the symbol is absent (a legacy plugin predating the tag) —
    // checkPluginContractTag treats that as a hard failure, since every plugin
    // shipped after the guard always exports it. PluginAbiMismatch only when no
    // .so has been loaded yet.
    [[nodiscard]] aid::plumbing::Result<std::optional<std::string>> contractTag() const;

private:
    static void nullDeleter(Port*) noexcept {}

    using Deleter = void (*)(Port*);

    void* handle_{nullptr};
    std::unique_ptr<Port, Deleter> instance_{nullptr, &nullDeleter};
};

template <class Port>
aid::plumbing::Result<void>
PluginLoader<Port>::load(std::string_view soPath, std::string_view createSym,
                         std::string_view destroySym, std::string_view configJson,
                         ::uid_t expectedOwnerUid) {
    using aid::plumbing::Error;
    using aid::plumbing::ErrorCode;

    const std::string pathStr{soPath};

    struct stat st {};
    if (::stat(pathStr.c_str(), &st) != 0) {
        return aid::plumbing::unexpected{
            Error{ErrorCode::PluginAbiMismatch,
                  "plugin stat " + pathStr + ": " + std::strerror(errno), std::nullopt}};
    }

    if ((st.st_mode & S_IWOTH) != 0) {
        return aid::plumbing::unexpected{
            Error{ErrorCode::PluginAbiMismatch,
                  "plugin " + pathStr + " is world-writable; refusing to load", std::nullopt}};
    }

    const ::gid_t daemonGid = ::getegid();
    if ((st.st_mode & S_IWGRP) != 0 && st.st_gid != daemonGid) {
        return aid::plumbing::unexpected{
            Error{ErrorCode::PluginAbiMismatch,
                  "plugin " + pathStr + " is group-writable by gid " + std::to_string(st.st_gid) +
                      " (daemon gid " + std::to_string(daemonGid) + "); refusing to load",
                  std::nullopt}};
    }

    if (st.st_uid != expectedOwnerUid) {
        return aid::plumbing::unexpected{
            Error{ErrorCode::PluginAbiMismatch,
                  "plugin " + pathStr + " owner uid " + std::to_string(st.st_uid) +
                      " does not match expected " + std::to_string(expectedOwnerUid) +
                      "; refusing to load",
                  std::nullopt}};
    }

    (void)::dlerror();
    void* handle = ::dlopen(pathStr.c_str(), RTLD_NOW);
    if (handle == nullptr) {
        const char* err = ::dlerror();
        return aid::plumbing::unexpected{Error{
            ErrorCode::PluginAbiMismatch,
            "dlopen " + pathStr + ": " + (err != nullptr ? err : "unknown error"), std::nullopt}};
    }

    // From here until handle_ = handle below, an early return on a
    // dlsym / factory failure deliberately leaks the freshly dlopen'd
    // handle. We do not call dlclose. The handle is process-lifetime; the operator
    // resolves a load error by restarting the daemon, at which point
    // the OS reclaims the mapping.

    const std::string createSymStr{createSym};
    (void)::dlerror();
    void* createPtr = ::dlsym(handle, createSymStr.c_str());
    const char* createErr = ::dlerror();
    if (createPtr == nullptr || createErr != nullptr) {
        return aid::plumbing::unexpected{
            Error{ErrorCode::PluginAbiMismatch,
                  "dlsym " + createSymStr + ": " +
                      (createErr != nullptr ? createErr : "symbol not found"),
                  std::nullopt}};
    }

    const std::string destroySymStr{destroySym};
    (void)::dlerror();
    void* destroyPtr = ::dlsym(handle, destroySymStr.c_str());
    const char* destroyErr = ::dlerror();
    if (destroyPtr == nullptr || destroyErr != nullptr) {
        return aid::plumbing::unexpected{
            Error{ErrorCode::PluginAbiMismatch,
                  "dlsym " + destroySymStr + ": " +
                      (destroyErr != nullptr ? destroyErr : "symbol not found"),
                  std::nullopt}};
    }

    using Factory = Port* (*)(const char*);
    Factory factory{};
    // POSIX dlsym returns void*; converting object↔function pointer is
    // conditionally supported by ISO C++ but well-defined by POSIX.
    // memcpy avoids the -Wpedantic complaint a reinterpret_cast would draw.
    std::memcpy(&factory, &createPtr, sizeof(factory));

    Deleter destroyer{};
    std::memcpy(&destroyer, &destroyPtr, sizeof(destroyer));

    const std::string configStr{configJson};
    Port* raw = factory(configStr.c_str());
    if (raw == nullptr) {
        return aid::plumbing::unexpected{
            Error{ErrorCode::PluginAbiMismatch,
                  "factory " + createSymStr + " returned nullptr for " + pathStr, std::nullopt}};
    }

    handle_ = handle; // intentionally leaked at process exit (no dlclose)
    instance_ = std::unique_ptr<Port, Deleter>{raw, destroyer};
    return {};
}

template <class Port>
aid::plumbing::Result<void>
PluginLoader<Port>::loadWithLoop(std::string_view soPath, std::string_view createSym,
                                 std::string_view destroySym, std::string_view configJson,
                                 void* eventLoop, ::uid_t expectedOwnerUid) {
    using aid::plumbing::Error;
    using aid::plumbing::ErrorCode;

    const std::string pathStr{soPath};

    struct stat st {};
    if (::stat(pathStr.c_str(), &st) != 0) {
        return aid::plumbing::unexpected{
            Error{ErrorCode::PluginAbiMismatch,
                  "plugin stat " + pathStr + ": " + std::strerror(errno), std::nullopt}};
    }

    if ((st.st_mode & S_IWOTH) != 0) {
        return aid::plumbing::unexpected{
            Error{ErrorCode::PluginAbiMismatch,
                  "plugin " + pathStr + " is world-writable; refusing to load", std::nullopt}};
    }

    const ::gid_t daemonGid = ::getegid();
    if ((st.st_mode & S_IWGRP) != 0 && st.st_gid != daemonGid) {
        return aid::plumbing::unexpected{
            Error{ErrorCode::PluginAbiMismatch,
                  "plugin " + pathStr + " is group-writable by gid " + std::to_string(st.st_gid) +
                      " (daemon gid " + std::to_string(daemonGid) + "); refusing to load",
                  std::nullopt}};
    }

    if (st.st_uid != expectedOwnerUid) {
        return aid::plumbing::unexpected{
            Error{ErrorCode::PluginAbiMismatch,
                  "plugin " + pathStr + " owner uid " + std::to_string(st.st_uid) +
                      " does not match expected " + std::to_string(expectedOwnerUid) +
                      "; refusing to load",
                  std::nullopt}};
    }

    (void)::dlerror();
    void* handle = ::dlopen(pathStr.c_str(), RTLD_NOW);
    if (handle == nullptr) {
        const char* err = ::dlerror();
        return aid::plumbing::unexpected{Error{
            ErrorCode::PluginAbiMismatch,
            "dlopen " + pathStr + ": " + (err != nullptr ? err : "unknown error"), std::nullopt}};
    }

    // From here until handle_ = handle below, an early return on a
    // dlsym / factory failure deliberately leaks the freshly dlopen'd
    // handle (no dlclose). Same
    // rationale as the classic load() path above.

    const std::string createSymStr{createSym};
    (void)::dlerror();
    void* createPtr = ::dlsym(handle, createSymStr.c_str());
    const char* createErr = ::dlerror();
    if (createPtr == nullptr || createErr != nullptr) {
        return aid::plumbing::unexpected{
            Error{ErrorCode::PluginAbiMismatch,
                  "dlsym " + createSymStr + ": " +
                      (createErr != nullptr ? createErr : "symbol not found"),
                  std::nullopt}};
    }

    const std::string destroySymStr{destroySym};
    (void)::dlerror();
    void* destroyPtr = ::dlsym(handle, destroySymStr.c_str());
    const char* destroyErr = ::dlerror();
    if (destroyPtr == nullptr || destroyErr != nullptr) {
        return aid::plumbing::unexpected{
            Error{ErrorCode::PluginAbiMismatch,
                  "dlsym " + destroySymStr + ": " +
                      (destroyErr != nullptr ? destroyErr : "symbol not found"),
                  std::nullopt}};
    }

    using FactoryWithLoop = Port* (*)(const char*, void*);
    FactoryWithLoop factory{};
    std::memcpy(&factory, &createPtr, sizeof(factory));

    Deleter destroyer{};
    std::memcpy(&destroyer, &destroyPtr, sizeof(destroyer));

    const std::string configStr{configJson};
    Port* raw = factory(configStr.c_str(), eventLoop);
    if (raw == nullptr) {
        return aid::plumbing::unexpected{
            Error{ErrorCode::PluginAbiMismatch,
                  "factory " + createSymStr + " returned nullptr for " + pathStr, std::nullopt}};
    }

    handle_ = handle; // intentionally leaked at process exit (no dlclose)
    instance_ = std::unique_ptr<Port, Deleter>{raw, destroyer};
    return {};
}

template <class Port>
aid::plumbing::Result<std::optional<int>> PluginLoader<Port>::apiVersion() const {
    using aid::plumbing::Error;
    using aid::plumbing::ErrorCode;

    if (handle_ == nullptr) {
        return aid::plumbing::unexpected{Error{
            ErrorCode::PluginAbiMismatch, "PluginLoader::apiVersion before load", std::nullopt}};
    }

    (void)::dlerror();
    void* sym = ::dlsym(handle_, "aid_plugin_api_version");
    if (sym == nullptr) {
        return std::optional<int>{}; // symbol absent — older plugin, no handshake
    }

    using VersionFn = int (*)();
    VersionFn fn{};
    std::memcpy(&fn, &sym, sizeof(fn));
    return std::optional<int>{fn()};
}

template <class Port>
aid::plumbing::Result<std::optional<std::uint64_t>> PluginLoader<Port>::abiLayoutTag() const {
    using aid::plumbing::Error;
    using aid::plumbing::ErrorCode;

    if (handle_ == nullptr) {
        return aid::plumbing::unexpected{Error{
            ErrorCode::PluginAbiMismatch, "PluginLoader::abiLayoutTag before load", std::nullopt}};
    }

    (void)::dlerror();
    void* sym = ::dlsym(handle_, "aid_plugin_abi_layout_tag");
    if (sym == nullptr) {
        return std::optional<std::uint64_t>{}; // symbol absent — older plugin
    }

    using TagFn = std::uint64_t (*)();
    TagFn fn{};
    std::memcpy(&fn, &sym, sizeof(fn));
    return std::optional<std::uint64_t>{fn()};
}

template <class Port>
aid::plumbing::Result<std::optional<std::string>> PluginLoader<Port>::contractTag() const {
    using aid::plumbing::Error;
    using aid::plumbing::ErrorCode;

    if (handle_ == nullptr) {
        return aid::plumbing::unexpected{Error{
            ErrorCode::PluginAbiMismatch, "PluginLoader::contractTag before load", std::nullopt}};
    }

    (void)::dlerror();
    void* sym = ::dlsym(handle_, "aid_plugin_contract_tag");
    if (sym == nullptr) {
        return std::optional<std::string>{}; // symbol absent — legacy plugin
    }

    using TagFn = const char* (*)();
    TagFn fn{};
    std::memcpy(&fn, &sym, sizeof(fn));
    const char* tag = fn();
    if (tag == nullptr) {
        return std::optional<std::string>{}; // defensive: treat null as absent
    }
    return std::optional<std::string>{std::string{tag}};
}

// The plugin ABI version this daemon build speaks. Both shipped
// plugins export aid_plugin_api_version() -> 1.
inline constexpr int kExpectedPluginApiVersion = 1;

// Load-time handshake: verify a freshly-loaded plugin's reported ABI
// version is compatible with this daemon. Pass the PluginLoader::apiVersion()
// result directly. Semantics:
//   - outer Result carries an error  -> propagate it (e.g. called before load).
//   - empty optional (symbol absent) -> OK: the handshake is optional-but-
//     recommended, so an older plugin without the
//     symbol is allowed to load.
//   - present but != expected        -> PluginAbiMismatch; Main refuses to start.
[[nodiscard]] inline aid::plumbing::Result<void>
checkPluginApiVersion(const aid::plumbing::Result<std::optional<int>>& apiVer,
                      std::string_view pluginName, int expected = kExpectedPluginApiVersion) {
    using aid::plumbing::Error;
    using aid::plumbing::ErrorCode;

    if (!apiVer) {
        return aid::plumbing::unexpected{apiVer.error()};
    }
    if (!apiVer->has_value()) {
        return {}; // no version handshake — allowed
    }
    const int reported = **apiVer;
    if (reported != expected) {
        return aid::plumbing::unexpected{
            Error{ErrorCode::PluginAbiMismatch,
                  std::string{pluginName} + ": plugin API version " + std::to_string(reported) +
                      " incompatible (daemon expects " + std::to_string(expected) + ")",
                  std::nullopt}};
    }
    return {};
}

namespace detail {
// Render a 64-bit tag as "0x" + 16 lowercase hex digits — std::to_string has
// no hex form and we keep this header free of <sstream>/<format> includes.
inline std::string toHex64(std::uint64_t value) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out = "0x0000000000000000";
    for (std::size_t i = 0; i < 16; ++i) {
        out[2 + (15 - i)] = kDigits[value & 0xFULL];
        value >>= 4;
    }
    return out;
}
} // namespace detail

// Load-time layout guard: verify a freshly-loaded plugin's value-type
// layout fingerprint matches this daemon's aid::abi::kPluginAbiLayoutTag. Pass
// the PluginLoader::abiLayoutTag() result directly. Unlike the version
// handshake, an ABSENT symbol is a HARD FAILURE: both shipped plugins always
// export aid_plugin_abi_layout_tag(), so absence means a stale or untrusted
// .so the daemon must not load (it would risk the heap-corruption SIGSEGV this
// guard exists to prevent). Semantics:
//   - outer Result carries an error  -> propagate it (e.g. called before load).
//   - empty optional (symbol absent) -> PluginAbiMismatch (refuse).
//   - present but != expected        -> PluginAbiMismatch (refuse).
[[nodiscard]] inline aid::plumbing::Result<void>
checkPluginAbiLayoutTag(const aid::plumbing::Result<std::optional<std::uint64_t>>& tag,
                        std::string_view pluginName,
                        std::uint64_t expected = aid::abi::kPluginAbiLayoutTag) {
    using aid::plumbing::Error;
    using aid::plumbing::ErrorCode;

    if (!tag) {
        return aid::plumbing::unexpected{tag.error()};
    }
    if (!tag->has_value()) {
        return aid::plumbing::unexpected{Error{
            ErrorCode::PluginAbiMismatch,
            std::string{pluginName} +
                ": plugin exports no ABI layout tag — rebuild and redeploy this plugin against "
                "the current daemon headers",
            std::nullopt}};
    }
    const std::uint64_t reported = **tag;
    if (reported != expected) {
        return aid::plumbing::unexpected{
            Error{ErrorCode::PluginAbiMismatch,
                  std::string{pluginName} + ": plugin ABI layout tag " + detail::toHex64(reported) +
                      " incompatible (daemon expects " + detail::toHex64(expected) +
                      ") — value-type layout skew; rebuild and redeploy this plugin",
                  std::nullopt}};
    }
    return {};
}

// BF3 load-time staleness guard: verify a freshly-loaded plugin's behaviour-
// contract tag matches this daemon's aid::abi::kPluginContractTag. Pass the
// PluginLoader::contractTag() result directly. Like the layout guard (and
// unlike the optional version handshake), an ABSENT tag is a HARD FAILURE:
// every plugin shipped after this guard exports aid_plugin_contract_tag(), so
// absence means a stale/legacy `.so` lagging the daemon — exactly the silent
// degraded-behaviour trap this guard exists to stop. Semantics:
//   - outer Result carries an error  -> propagate it (e.g. called before load).
//   - empty optional (symbol absent) -> PluginAbiMismatch (refuse).
//   - present but != expected        -> PluginAbiMismatch (refuse).
[[nodiscard]] inline aid::plumbing::Result<void>
checkPluginContractTag(const aid::plumbing::Result<std::optional<std::string>>& tag,
                       std::string_view pluginName,
                       std::string_view expected = aid::abi::kPluginContractTag) {
    using aid::plumbing::Error;
    using aid::plumbing::ErrorCode;

    if (!tag) {
        return aid::plumbing::unexpected{tag.error()};
    }
    if (!tag->has_value()) {
        return aid::plumbing::unexpected{
            Error{ErrorCode::PluginAbiMismatch,
                  std::string{pluginName} +
                      ": plugin exports no behaviour-contract tag — this is a stale/legacy .so; "
                      "rebuild and redeploy it against the current daemon (scripts/deploy.sh)",
                  std::nullopt}};
    }
    if (**tag != expected) {
        return aid::plumbing::unexpected{
            Error{ErrorCode::PluginAbiMismatch,
                  std::string{pluginName} + ": plugin contract tag '" + **tag +
                      "' incompatible (daemon expects '" + std::string{expected} +
                      "') — stale plugin lagging the daemon; rebuild and redeploy it",
                  std::nullopt}};
    }
    return {};
}

} // namespace aid::infrastructure
