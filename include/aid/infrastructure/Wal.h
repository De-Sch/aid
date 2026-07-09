#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "aid/plumbing/Result.h"
#include "aid/plumbing/WalRecord.h"

namespace aid::crosscutting {
class Clock;
}

namespace aid::infrastructure {

class Wal {
public:
    Wal(std::string path, aid::crosscutting::Clock& clock);
    Wal(const Wal&) = delete;
    Wal& operator=(const Wal&) = delete;
    Wal(Wal&&) = delete;
    Wal& operator=(Wal&&) = delete;
    // Virtual so the test seam (syncToDisk) can be overridden in a subclass
    // without tripping -Wnon-virtual-dtor. Wal is never owned through a base
    // pointer in production; this costs nothing observable.
    virtual ~Wal();

    [[nodiscard]] std::vector<aid::plumbing::WalRecord> readAll();

    [[nodiscard]] aid::plumbing::Result<std::uint64_t> append(std::string_view jsonBody,
                                                              std::string_view correlationId);

    // Acknowledge that the single record with this seq has been durably
    // handled downstream. Only the longest CONTIGUOUS acked prefix is ever
    // physically removed from the front of the log, so an un-acked lower seq
    // (from a different, still-in-flight callid) is never dropped. Idempotent.
    [[nodiscard]] aid::plumbing::Result<void> ack(std::uint64_t seq);

    [[nodiscard]] std::uint64_t pendingCount() const noexcept;

    [[nodiscard]] static std::string toLine(const aid::plumbing::WalRecord& r);

    [[nodiscard]] static std::optional<aid::plumbing::WalRecord> parseLine(std::string_view line);

protected:
    // Durability primitive: flush the just-appended line to stable
    // storage. Virtual purely as a test seam — a subclass can force a sync
    // failure to exercise the WalSyncFailed path without faulting a real disk.
    // Production behaviour is a plain ::fdatasync(fd); the indirection costs one
    // non-inlined virtual call per append (sub-µs, dwarfed by the sync itself).
    [[nodiscard]] virtual int syncToDisk(int fd) noexcept;

private:
    std::string path_;
    aid::crosscutting::Clock& clock_;
    mutable std::mutex writeMtx_;
    int fd_{-1};
    std::atomic<std::uint64_t> nextSeq_{1};
    std::vector<aid::plumbing::WalRecord> pendingReplay_;

    // Acknowledgement state, guarded by writeMtx_. `ackedPrefix_` is the
    // highest seq such that every seq <= it is acked or already compacted
    // away; on disk the file holds only records with seq > ackedPrefix_.
    // `ackedAhead_` holds acked seqs that sit above the contiguous prefix
    // (their lower neighbours are still in flight). When the prefix advances
    // to cover them they are erased from this set.
    std::set<std::uint64_t> ackedAhead_;
    std::uint64_t ackedPrefix_{0};

    // Rewrites the log dropping every record with seq <= dropUpTo. Assumes
    // writeMtx_ is already held by the caller.
    [[nodiscard]] aid::plumbing::Result<void> rewriteDroppingUpToLocked(std::uint64_t dropUpTo);
};

} // namespace aid::infrastructure
