#include "Ui/Ui.h"

Ui::Ui(nlohmann::json &config) {}
Ui::~Ui() {}

template <typename T>
T Ui::getConfigValue(nlohmann::json &config, const char *param,
                     const T &defaultVal, bool &hasError) {
  try {
    return config[param];
  } catch (nlohmann::json::exception &e) {
    config[param] = defaultVal;
    hasError = true;
    return defaultVal;
  }
}
