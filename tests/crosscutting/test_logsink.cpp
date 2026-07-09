#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "aid/crosscutting/LogSink.h"

namespace fs = std::filesystem;

namespace {

fs::path uniqueTempDir(std::string_view stem) {
    static std::atomic<uint64_t> counter{0};
    const auto pid = static_cast<uint64_t>(::getpid());
    const auto seq = counter.fetch_add(1, std::memory_order_relaxed);
    const auto now =
        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    std::ostringstream name;
    name << stem << "_" << pid << "_" << now << "_" << seq;
    auto p = fs::temp_directory_path() / name.str();
    fs::create_directories(p);
    return p;
}

std::string readWhole(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

using aid::crosscutting::LogSink;

TEST(LogSink, DefaultConstructedIsClosed) {
    LogSink s;
    EXPECT_FALSE(s.isOpen());
    EXPECT_TRUE(s.path().empty());
}

TEST(LogSink, OpenCreatesParentDirectories) {
    auto base = uniqueTempDir("logsink_parents");
    auto p = base / "a" / "b" / "c" / "test.log";

    LogSink s;
    s.open(p.string());

    EXPECT_TRUE(s.isOpen());
    EXPECT_EQ(s.path(), p.string());
    EXPECT_TRUE(fs::exists(p));
}

TEST(LogSink, WriteAppendsLineAndFlushes) {
    auto base = uniqueTempDir("logsink_write");
    auto p = base / "out.log";

    LogSink s;
    s.open(p.string());
    s.write("hello");

    EXPECT_EQ(readWhole(p), "hello\n");

    s.write("world");
    EXPECT_EQ(readWhole(p), "hello\nworld\n");
}

TEST(LogSink, ReopenAppendsRatherThanTruncates) {
    auto base = uniqueTempDir("logsink_reopen");
    auto p = base / "out.log";

    {
        LogSink s;
        s.open(p.string());
        s.write("first");
    }
    {
        LogSink s;
        s.open(p.string());
        s.write("second");
    }

    EXPECT_EQ(readWhole(p), "first\nsecond\n");
}

TEST(LogSink, OpenUnderRegularFileReportsFailure) {
    auto base = uniqueTempDir("logsink_badpath");
    auto blocker = base / "blocker";
    { std::ofstream(blocker) << "x"; }
    // 'blocker' is a regular file; using it as a directory must fail. Whether
    // the failure surfaces as a thrown exception or as a closed sink depends
    // on the libstdc++ / libc filesystem implementation — accept either, but
    // never silent success.
    auto p = blocker / "out.log";

    LogSink s;
    bool threw = false;
    try {
        s.open(p.string());
    } catch (const std::exception&) {
        threw = true;
    }
    EXPECT_TRUE(threw || !s.isOpen());
}

TEST(LogSink, ConcurrentWritesProduceCompleteLines) {
    auto base = uniqueTempDir("logsink_concurrent");
    auto p = base / "out.log";

    LogSink s;
    s.open(p.string());

    constexpr int kThreads = 8;
    constexpr int kPerThread = 64;
    std::vector<std::thread> ts;
    ts.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([t, &s] {
            for (int i = 0; i < kPerThread; ++i) {
                std::ostringstream o;
                o << "t" << t << "-i" << i;
                s.write(o.str());
            }
        });
    }
    for (auto& th : ts)
        th.join();

    std::ifstream f(p);
    std::set<std::string> seen;
    std::string line;
    int count = 0;
    while (std::getline(f, line)) {
        EXPECT_FALSE(line.empty());
        // Each line must be one full "tX-iY" token, never interleaved.
        EXPECT_EQ(line.find('-'), line.rfind('-'));
        EXPECT_EQ(line[0], 't');
        seen.insert(line);
        ++count;
    }
    EXPECT_EQ(count, kThreads * kPerThread);
    EXPECT_EQ(static_cast<int>(seen.size()), kThreads * kPerThread);
}
