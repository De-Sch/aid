#pragma once

#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace aid::crosscutting {

class LogSink {
public:
    LogSink() = default;
    LogSink(const LogSink&) = delete;
    LogSink& operator=(const LogSink&) = delete;
    LogSink(LogSink&&) = delete;
    LogSink& operator=(LogSink&&) = delete;

    void open(std::string_view path);

    void write(std::string_view line);

    [[nodiscard]] bool isOpen() const;

    // Returned by value (under the mutex) so a concurrent open() that
    // reassigns path_ can never tear a string under the reader.
    [[nodiscard]] std::string path() const;

private:
    mutable std::mutex mtx_;
    std::ofstream file_;
    std::string path_;
};

} // namespace aid::crosscutting
