#include "aid/infrastructure/Wal.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>

#include "aid/crosscutting/Clock.h"
#include "aid/crosscutting/Logger.h"
#include "aid/plumbing/Error.h"

namespace aid::infrastructure {

namespace {

using aid::plumbing::Error;
using aid::plumbing::ErrorCode;
using aid::plumbing::WalRecord;

std::string formatTs(std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;
    const auto secs = time_point_cast<seconds>(tp);
    const auto millis = duration_cast<milliseconds>(tp - secs).count();

    std::time_t t = system_clock::to_time_t(secs);
    std::tm utc{};
    ::gmtime_r(&t, &utc);

    char head[32];
    const std::size_t n = std::strftime(head, sizeof head, "%Y-%m-%dT%H:%M:%S", &utc);
    if (n == 0) {
        return std::string{"1970-01-01T00:00:00.000Z"};
    }

    char tail[12];
    std::snprintf(tail, sizeof tail, ".%03lldZ", static_cast<long long>(millis));

    std::string out;
    out.reserve(n + std::strlen(tail));
    out.append(head, n);
    out.append(tail);
    return out;
}

std::optional<std::chrono::system_clock::time_point> parseTs(const std::string& s) {
    int Y = 0, M = 0, D = 0, h = 0, m = 0, sec = 0, ms = 0;
    char tz = 0;
    int parsed =
        std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d.%d%c", &Y, &M, &D, &h, &m, &sec, &ms, &tz);
    if (parsed != 8) {
        ms = 0;
        parsed = std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d%c", &Y, &M, &D, &h, &m, &sec, &tz);
        if (parsed != 7) {
            return std::nullopt;
        }
    }
    if (tz != 'Z') {
        return std::nullopt;
    }
    if (Y < 1970 || M < 1 || M > 12 || D < 1 || D > 31 || h < 0 || h > 23 || m < 0 || m > 59 ||
        sec < 0 || sec > 60 || ms < 0 || ms > 999) {
        return std::nullopt;
    }

    std::tm utc{};
    utc.tm_year = Y - 1900;
    utc.tm_mon = M - 1;
    utc.tm_mday = D;
    utc.tm_hour = h;
    utc.tm_min = m;
    utc.tm_sec = sec;
    const std::time_t t = ::timegm(&utc);
    if (t == static_cast<std::time_t>(-1)) {
        return std::nullopt;
    }
    return std::chrono::system_clock::time_point{std::chrono::seconds{t} +
                                                 std::chrono::milliseconds{ms}};
}

Error walWriteError(const char* what, std::string_view cid = {}) {
    Error e{ErrorCode::WalWriteFailed, std::string{what} + ": " + std::strerror(errno),
            std::nullopt};
    if (!cid.empty()) {
        e.correlationId = std::string{cid};
    }
    return e;
}

Error walSyncError(const char* what, std::string_view cid = {}) {
    Error e{ErrorCode::WalSyncFailed, std::string{what} + ": " + std::strerror(errno),
            std::nullopt};
    if (!cid.empty()) {
        e.correlationId = std::string{cid};
    }
    return e;
}

class ScopedFd {
public:
    explicit ScopedFd(int fd) noexcept : fd_(fd) {}
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    ScopedFd(ScopedFd&&) = delete;
    ScopedFd& operator=(ScopedFd&&) = delete;
    ~ScopedFd() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

private:
    int fd_;
};

bool writeAll(int fd, const char* data, std::size_t len) {
    std::size_t off = 0;
    while (off < len) {
        const ssize_t n = ::write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

[[noreturn]] void throwErrno(const std::string& what) {
    throw std::runtime_error(what + ": " + std::strerror(errno));
}

} // namespace

std::string Wal::toLine(const WalRecord& r) {
    nlohmann::json j;
    j["seq"] = r.seq;
    j["ts"] = formatTs(r.receivedAt);
    j["cid"] = r.correlationId;
    j["body"] = r.body;
    std::string out = j.dump();
    out.push_back('\n');
    return out;
}

std::optional<WalRecord> Wal::parseLine(std::string_view line) {
    auto j = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        return std::nullopt;
    }
    if (!j.contains("seq") || !j.contains("ts") || !j.contains("cid") || !j.contains("body")) {
        return std::nullopt;
    }
    const auto& jseq = j["seq"];
    const auto& jts = j["ts"];
    const auto& jcid = j["cid"];
    const auto& jbody = j["body"];
    if (!jseq.is_number_unsigned() && !(jseq.is_number_integer() && jseq.get<long long>() >= 0)) {
        return std::nullopt;
    }
    if (!jts.is_string() || !jcid.is_string() || !jbody.is_string()) {
        return std::nullopt;
    }
    const auto ts = parseTs(jts.get<std::string>());
    if (!ts) {
        return std::nullopt;
    }
    WalRecord r;
    r.seq = jseq.get<std::uint64_t>();
    r.receivedAt = *ts;
    r.correlationId = jcid.get<std::string>();
    r.body = jbody.get<std::string>();
    return r;
}

Wal::Wal(std::string path, aid::crosscutting::Clock& clock)
    : path_(std::move(path)), clock_(clock) {
    const std::filesystem::path p{path_};
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
        if (ec) {
            throw std::runtime_error("Wal: create parent dir " + p.parent_path().string() + ": " +
                                     ec.message());
        }
    }

    const bool existed = std::filesystem::exists(p);

    std::uint64_t maxSeq = 0;
    std::uint64_t minSeq = 0;
    bool sawRecord = false;
    if (existed) {
        std::ifstream in(path_, std::ios::binary);
        if (!in) {
            throw std::runtime_error("Wal: cannot open existing log " + path_ +
                                     " (permission misconfiguration?)");
        }
        std::string line;
        std::size_t lineNo = 0;
        while (std::getline(in, line)) {
            ++lineNo;
            if (line.empty()) {
                continue;
            }
            auto rec = parseLine(line);
            if (!rec) {
                aid::crosscutting::Logger::instance().warn("WAL: skipping malformed line " +
                                                           std::to_string(lineNo) + " in " + path_);
                continue;
            }
            if (rec->seq > maxSeq) {
                maxSeq = rec->seq;
            }
            if (!sawRecord || rec->seq < minSeq) {
                minSeq = rec->seq;
            }
            sawRecord = true;
            pendingReplay_.push_back(std::move(*rec));
        }
    }

    nextSeq_.store(maxSeq + 1, std::memory_order_relaxed);

    // Replay floor: any seq below the smallest record still on disk was
    // compacted away by a prior run, so it counts as already acked. ack()
    // never advances the prefix past a malformed/missing seq, so a corrupt
    // middle line caps compaction (no data loss — see ack()).
    ackedPrefix_ = sawRecord ? minSeq - 1 : 0;

    fd_ = ::open(path_.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (fd_ < 0) {
        throwErrno("Wal: open(" + path_ + ", O_WRONLY|O_APPEND|O_CREAT)");
    }

    // First-ever creation: fsync the parent directory so the new inbox.log
    // directory entry survives a power loss before the first append's dir
    // sync. Without this, strict POSIX permits the file to vanish on crash.
    // Mirrors the dir fsync in rewriteDroppingUpToLocked(); a no-op cost on
    // every later restart since we only do it when the file did not exist.
    if (!existed && p.has_parent_path()) {
        ScopedFd dirFd(::open(p.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (dirFd.valid() && ::fsync(dirFd.get()) != 0) {
            throwErrno("Wal: fsync parent dir " + p.parent_path().string());
        }
    }
}

Wal::~Wal() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

std::vector<WalRecord> Wal::readAll() {
    return std::move(pendingReplay_);
}

aid::plumbing::Result<std::uint64_t> Wal::append(std::string_view jsonBody,
                                                 std::string_view correlationId) {
    std::lock_guard lock{writeMtx_};

    const auto seq = nextSeq_.fetch_add(1, std::memory_order_relaxed);
    WalRecord rec{seq, clock_.now(), std::string{correlationId}, std::string{jsonBody}};
    const std::string line = toLine(rec);

    if (!writeAll(fd_, line.data(), line.size())) {
        return aid::plumbing::unexpected{walWriteError("write", correlationId)};
    }
    if (syncToDisk(fd_) != 0) {
        return aid::plumbing::unexpected{walSyncError("fdatasync", correlationId)};
    }
    return seq;
}

int Wal::syncToDisk(int fd) noexcept {
    return ::fdatasync(fd);
}

aid::plumbing::Result<void> Wal::ack(std::uint64_t seq) {
    std::lock_guard lock{writeMtx_};

    // Already compacted away (or below the replay floor): idempotent no-op.
    if (seq <= ackedPrefix_) {
        return {};
    }

    // Callids run in parallel over one shared seq counter, so acks
    // arrive out of order. Record this ack and advance the contiguous prefix
    // only as far as an unbroken run of acked seqs allows. A still-in-flight
    // lower seq (from another callid) stops the advance, so it is never
    // physically dropped — preserving the at-least-once guarantee.
    ackedAhead_.insert(seq);
    const std::uint64_t prevPrefix = ackedPrefix_;
    while (ackedAhead_.erase(ackedPrefix_ + 1) != 0) {
        ++ackedPrefix_;
    }

    // No advance => nothing new can be dropped from the front; skip the
    // file rewrite entirely (also keeps acking out-of-order seqs cheap).
    if (ackedPrefix_ == prevPrefix) {
        return {};
    }

    return rewriteDroppingUpToLocked(ackedPrefix_);
}

aid::plumbing::Result<void> Wal::rewriteDroppingUpToLocked(std::uint64_t seq) {
    const std::string tmpPath = path_ + ".tmp";

    // ScopedFd owns the tmp file across any throw in the rewrite loop
    // (std::bad_alloc from getline/push_back). The dtor closes the fd;
    // we delete the stale tmp file on any error path before returning.
    {
        ScopedFd tmp(::open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
        if (!tmp.valid()) {
            return aid::plumbing::unexpected{walWriteError("open tmp")};
        }

        std::ifstream in(path_, std::ios::binary);
        if (in) {
            std::string line;
            while (std::getline(in, line)) {
                if (line.empty()) {
                    continue;
                }
                const auto rec = parseLine(line);
                if (!rec) {
                    continue;
                }
                if (rec->seq <= seq) {
                    continue;
                }
                line.push_back('\n');
                if (!writeAll(tmp.get(), line.data(), line.size())) {
                    std::error_code ec;
                    std::filesystem::remove(tmpPath, ec);
                    return aid::plumbing::unexpected{walWriteError("truncate rewrite")};
                }
            }
        }

        if (::fdatasync(tmp.get()) != 0) {
            std::error_code ec;
            std::filesystem::remove(tmpPath, ec);
            return aid::plumbing::unexpected{walSyncError("fdatasync tmp")};
        }
    } // tmp fd closed here, before rename

    std::error_code ec;
    std::filesystem::rename(tmpPath, path_, ec);
    if (ec) {
        std::filesystem::remove(tmpPath, ec);
        return aid::plumbing::unexpected{
            Error{ErrorCode::WalWriteFailed, "rename: " + ec.message(), std::nullopt}};
    }

    // Fsync the parent directory so the rename survives a power loss.
    // Without this, POSIX permits the directory entry to be lost on crash
    // and the daemon would replay events that were already acked.
    {
        const auto parentDir = std::filesystem::path{path_}.parent_path();
        if (!parentDir.empty()) {
            ScopedFd dirFd(::open(parentDir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
            if (dirFd.valid() && ::fsync(dirFd.get()) != 0) {
                return aid::plumbing::unexpected{walSyncError("fsync parent dir")};
            }
        }
    }

    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    fd_ = ::open(path_.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (fd_ < 0) {
        return aid::plumbing::unexpected{walWriteError("reopen after truncate")};
    }

    return {};
}

std::uint64_t Wal::pendingCount() const noexcept {
    return pendingReplay_.size();
}

} // namespace aid::infrastructure
