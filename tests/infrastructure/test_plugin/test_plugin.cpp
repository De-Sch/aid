#include <cstdint>
#include <string>

#include "aid/abi/PluginAbiTag.h"
#include "aid/abi/PluginContract.h"
#include "test_port.h"

namespace {

class TestPortImpl final : public TestPort {
public:
    explicit TestPortImpl(std::string cfg) : cfg_(std::move(cfg)) {}

    [[nodiscard]] int answer() const override { return 42; }
    [[nodiscard]] std::string echoConfig() const override { return cfg_; }

private:
    std::string cfg_;
};

} // namespace

extern "C" TestPort* create_TestPort(const char* config_json) {
    return new TestPortImpl{config_json != nullptr ? std::string{config_json} : std::string{}};
}

extern "C" void destroy_TestPort(TestPort* p) {
    delete p;
}

extern "C" int aid_plugin_api_version(void) {
    return 1;
}

// Built from the same value-type headers as the daemon/test exe, so this
// matches aid::abi::kPluginAbiLayoutTag — exercises the loader's match path.
extern "C" std::uint64_t aid_plugin_abi_layout_tag(void) {
    return aid::abi::kPluginAbiLayoutTag;
}

// BF3 stale-plugin guard: built from the same header as the daemon/test exe, so
// it matches aid::abi::kPluginContractTag — exercises the loader's match path.
extern "C" const char* aid_plugin_contract_tag(void) {
    return aid::abi::kPluginContractTag;
}
