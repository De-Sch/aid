#include "aid/crosscutting/LogSink.h"

#include <filesystem>
#include <ios>
#include <stdexcept>
#include <system_error>

namespace aid::crosscutting {

void LogSink::open(std::string_view path) {
    std::lock_guard lock(mtx_);

    std::filesystem::path p{path};
    if (auto parent = p.parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            throw std::runtime_error("LogSink::open: cannot create parent of " + std::string{path} +
                                     ": " + ec.message());
        }
    }

    // ofstream::open is a no-op on an already-open stream — close first so
    // re-opening against a new path (the singleton-reinitialization path) is
    // observable.
    if (file_.is_open()) {
        file_.close();
    }
    file_.clear();
    file_.open(p, std::ios::out | std::ios::app);
    if (!file_.is_open()) {
        throw std::runtime_error("LogSink::open: cannot open " + std::string{path});
    }
    path_.assign(path);
}

void LogSink::write(std::string_view line) {
    std::lock_guard lock(mtx_);
    if (!file_.is_open())
        return;
    file_.write(line.data(), static_cast<std::streamsize>(line.size()));
    file_.put('\n');
    file_.flush();
}

bool LogSink::isOpen() const {
    std::lock_guard lock(mtx_);
    return file_.is_open();
}

std::string LogSink::path() const {
    std::lock_guard lock(mtx_);
    return path_;
}

} // namespace aid::crosscutting
