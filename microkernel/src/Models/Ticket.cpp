/**
 * @file Ticket.cpp
 * @brief Implementation of Ticket abstract base class constructors and helpers
 *
 * Provides:
 * - Two constructors (minimal and config-based)
 * - Template method for safe config value extraction
 * - Logging for plugin initialization diagnostics
 *
 * @see Ticket.h for detailed documentation
 */

#include "Models/Ticket.h"
#include "Logger/Logger.h"
#include <string>

/**
 * Minimal constructor - only stores TicketSystem reference
 * Used when ticket is instantiated without config (rare case)
 */
Ticket::Ticket(const TicketSystem &api) : api(api) {}

/**
 * Primary constructor - loads plugin configuration and validates settings
 *
 * Business Logic:
 * - Initializes userInformation from TicketSystem config (default assignee)
 * - Logs library path for debugging plugin loading issues
 * - Validates required config fields and logs errors if missing
 * - If config missing, writes default template values for operator to fix
 */
Ticket::Ticket(nlohmann::json &config, const TicketSystem &api) : api(api) {
  bool hasConfigError = false;

  // Initialize default user from system config
  userInformation = api.configUser;

  // Log plugin loading attempt (helps diagnose dlopen failures)
  const std::string libPath =
      getConfigValue(config, "libPath", std::string(""), hasConfigError);
  logging::Logger::info("Try to load Ticket dll from: " + libPath);

  // Log plugin name for identification
  const std::string ticketName =
      getConfigValue(config, "ticketName", std::string(""), hasConfigError);
  logging::Logger::info("INFO: Ticket: " + ticketName);

  // Report configuration validation results
  if (!hasConfigError) {
    logging::Logger::info("Ticket loaded without issues");
  } else {
    logging::Logger::error(
        "Missing config values for Ticket, template has been written!");
  }
}

/**
 * Template method implementation for safe config value extraction
 *
 * Error Handling Strategy:
 * - Catches nlohmann::json::exception when field is missing or wrong type
 * - Writes default value back to config (creates template for missing fields)
 * - Sets hasError flag so caller can detect configuration issues
 * - Returns default value to allow graceful degradation
 *
 * This approach allows the system to start even with incomplete config,
 * while logging the issues for operator attention.
 */
template <typename T>
T Ticket::getConfigValue(nlohmann::json &config, const char *param,
                         const T &defaultVal, bool &hasError) {
  try {
    return config[param];
  } catch (nlohmann::json::exception &e) {
    // Missing or wrong type - write default and flag error
    config[param] = defaultVal;
    hasError = true;
    return defaultVal;
  }
}
