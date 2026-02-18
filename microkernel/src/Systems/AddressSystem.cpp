/**
 * @file AddressSystem.cpp
 * @brief Implementation of AddressSystem base class methods
 *
 * Provides base functionality for all AddressSystem implementations:
 * - Configuration loading from JSON
 * - Template method for safe config value extraction
 * - Default getDashboardInformation implementation
 *
 * @dependencies Logger (for error/info logging)
 */

#include "Systems/AddressSystem.h"
#include "Logger/Logger.h"
#include <iostream>

/**
 * @brief Constructor that loads base configuration from JSON
 *
 * Extracts common configuration parameters used by all AddressSystem
 * implementations:
 * - addressSystemName: Plugin identifier (e.g., plugin name from config)
 * - bookAddresses: URL/path to individual contacts address book
 * - bookCompanies: URL/path to company contacts address book
 * - user: Authentication username
 * - password: Authentication password
 *
 * Logs errors if any parameters are missing, but continues with default values.
 *
 * @param config JSON configuration object from config.json "AddressSystem"
 * section
 *
 * @note Derived classes should call this constructor in their initializer list
 * @note Logs to Logger::error if required config values are missing
 * @note Logs to Logger::info on successful config load
 */
AddressSystem::AddressSystem(nlohmann::json &config) {
  bool err = false;
  configAddressSystemName =
      getConfigValue(config, "addressSystemName", std::string(""), err);
  configBookDirectDial =
      getConfigValue(config, "bookAddresses", std::string(""), err);
  configBookCompanies =
      getConfigValue(config, "bookCompanies", std::string(""), err);
  configUser = getConfigValue(config, "user", std::string(""), err);
  configPassword = getConfigValue(config, "password", std::string(""), err);

  if (err) {
    logging::Logger::error("ERROR: Missing Config values for base "
                           "AddressSystem, some default values have been set.");
  } else {
    logging::Logger::info("Base AddressSystem config loaded successfully.");
    logging::Logger::debug("Base AddressSystem configUser: '" + configUser +
                           "'");
  }
}

/**
 * @brief Default implementation of getDashboardInformation
 *
 * Returns empty string by default. Derived classes can override this method
 * to provide custom dashboard data for the UI.
 *
 * @param payload Input stream containing request payload (JSON)
 * @param urlParams URL parameters from dashboard request
 *
 * @return std::string Empty string (no dashboard data)
 *
 * @note This is a virtual method with a default implementation
 * @note Override in derived classes to provide actual dashboard data
 */
std::string AddressSystem::getDashboardInformation(std::istream &payload,
                                                   std::string &urlParams) {
  return "";
}

/**
 * @brief Template method to safely extract configuration values from JSON
 *
 * Attempts to extract a value from the JSON config object. If the parameter
 * doesn't exist or causes an exception, returns the default value and logs
 * a warning message.
 *
 * Error Handling:
 * - Catches nlohmann::json::exception if parameter is missing
 * - Logs warning with parameter name and error details
 * - Adds missing parameter to config object with default value
 * - Sets hasError flag to true
 * - Returns default value
 *
 * @tparam T Type of the configuration parameter
 * @param config JSON configuration object to extract from
 * @param param Name of the parameter to extract
 * @param defaultVal Default value to use if parameter is missing
 * @param hasError Output flag - set to true if parameter was missing
 *
 * @return T The extracted value or defaultVal if not found
 *
 * @note This method modifies the config object by adding missing parameters
 * @note Always returns a value (never throws exceptions)
 */
template <typename T>
T AddressSystem::getConfigValue(nlohmann::json &config, const char *param,
                                const T &defaultVal, bool &hasError) {
  try {
    return config[param];
  } catch (nlohmann::json::exception &e) {
    logging::Logger::warn(
        "Missing config parameter '" + std::string(param) +
        "' in AddressSystem config, using default value. Error: " +
        std::string(e.what()));
    config[param] = defaultVal;
    hasError = true;
    return defaultVal;
  }
}
