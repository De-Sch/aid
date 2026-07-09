#pragma once

#include <string>

// TestPort — the abstract port used by tests/infrastructure/test_plugin and
// test_plugin_loader.cpp. Kept self-contained (no aid_ports dependency) so
// the test .so doesn't pull in value-types / plumbing transitively, mirroring
// the production plugin contract (extern "C" factory + matching abstract).

struct TestPort {
    TestPort() = default;
    TestPort(const TestPort&) = delete;
    TestPort& operator=(const TestPort&) = delete;
    TestPort(TestPort&&) = delete;
    TestPort& operator=(TestPort&&) = delete;
    virtual ~TestPort() = default;

    [[nodiscard]] virtual int answer() const = 0;
    [[nodiscard]] virtual std::string echoConfig() const = 0;
};

extern "C" TestPort* create_TestPort(const char* config_json);
extern "C" void destroy_TestPort(TestPort* p);
extern "C" int aid_plugin_api_version(void);
