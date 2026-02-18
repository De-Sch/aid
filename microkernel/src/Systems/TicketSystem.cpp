/**
 * @file TicketSystem.cpp
 * @brief Implementation of TicketSystem base class methods
 *
 * Provides base functionality for all TicketSystem implementations:
 * - Configuration loading from JSON
 * - Template method for safe config value extraction
 * - Call ID formatting and management utilities
 *
 * Call ID Management:
 * Tickets can be associated with multiple call IDs (comma-separated) to handle:
 * - Call-backs: New call for existing ticket
 * - Conference calls: Multiple simultaneous calls
 * - Call transfers: Call ID changes during transfer
 *
 * @dependencies Logger (for error/info logging), Ticket model
 */

#include "Systems/TicketSystem.h"
#include "Logger/Logger.h"
#include "Models/Ticket.h"
#include <iostream>

/**
 * @brief Constructor that loads base configuration from JSON
 *
 * Extracts common configuration parameters used by all TicketSystem
 * implementations:
 * - libPath: Path to plugin shared library (.so file)
 * - baseUrl: API base URL
 * - apiToken: Authentication token
 * - user: User resource URL (HAL+JSON format)
 * - fieldCallId: Custom field name for call ID storage
 * - fieldCallerNumber: Custom field name for caller phone number
 * - fieldCalledNumber: Custom field name for dialed number
 * - statusNew: Status ID for new tickets
 * - statusInProgress: Status ID for in-progress tickets
 * - statusClosed: Status ID for closed tickets
 * - unknownNumberSaveLocation: Project ID for unknown callers
 * - projectWebBaseUrl: Base URL for ticket web links
 * - fieldCallStart: Custom field for call start timestamp
 * - fieldCallEnd: Custom field for call end timestamp
 *
 * Logs errors if any parameters are missing, but continues with default values.
 *
 * @param config JSON configuration object from config.json "TicketSystem"
 * section
 *
 * @note Derived classes should call this constructor in their initializer list
 * @note Logs to Logger::error if required config values are missing
 * @note Logs to Logger::info on successful config load
 */
TicketSystem::TicketSystem(nlohmann::json &config) {
  bool err = false;
  configLibPath = getConfigValue(config, "libPath", std::string(""), err);
  configUrl = getConfigValue(config, "baseUrl", std::string(""), err);
  configApiToken = getConfigValue(config, "apiToken", std::string(""), err);
  configUser = getConfigValue(config, "user", std::string(""), err);
  configCallId = getConfigValue(config, "fieldCallId", std::string(""), err);
  configCallerNumber =
      getConfigValue(config, "fieldCallerNumber", std::string(""), err);
  configCalledNumber =
      getConfigValue(config, "fieldCalledNumber", std::string(""), err);
  configStatusNew = getConfigValue(config, "statusNew", std::string(""), err);
  configStatusInProgress =
      getConfigValue(config, "statusInProgress", std::string(""), err);
  configStatusClosed =
      getConfigValue(config, "statusClosed", std::string(""), err);
  configUnknownNumberSaveLocation =
      getConfigValue(config, "unknownNumberSaveLocation", std::string(""), err);
  configProjectWebBaseUrl =
      getConfigValue(config, "projectWebBaseUrl", std::string(""), err);
  configCallStartTimestamp =
      getConfigValue(config, "fieldCallStart", std::string(""), err);
  configCallEndTimestamp =
      getConfigValue(config, "fieldCallEnd", std::string(""), err);

  if (err) {
    logging::Logger::error("ERROR: Missing Config values for base "
                           "TicketSystem, some default values have been set.");
  } else {
    logging::Logger::info("Base TicketSystem config loaded successfully.");
    logging::Logger::debug("Base TicketSystem configStatusInProgress: '" +
                           configStatusInProgress + "'");
  }
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
T TicketSystem::getConfigValue(nlohmann::json &config, const char *param,
                               const T &defaultVal, bool &hasError) {
  try {
    return config[param];
  } catch (nlohmann::json::exception &e) {
    logging::Logger::warn(
        "Missing config parameter '" + std::string(param) +
        "' in TicketSystem config, using default value. Error: " +
        std::string(e.what()));
    config[param] = defaultVal;
    hasError = true;
    return defaultVal;
  }
}

/**
 * @brief Format a call ID for storage
 *
 * Appends ", " to the call ID to create a comma-separated list format.
 * This allows multiple call IDs to be stored in a single custom field.
 *
 * Example:
 * - Input: "1234567890.123"
 * - Output: "1234567890.123, "
 *
 * @param callId Raw call ID from Asterisk
 *
 * @return std::string Formatted call ID with trailing ", "
 *
 * @note Override this method to customize call ID formatting
 */
std::string TicketSystem::formatCallId(const std::string &callId) {
  return callId + ", ";
}

/**
 * @brief Add a call ID to an existing comma-separated list
 *
 * Appends a new call ID to the existing call IDs string if it's not already
 * present. Handles three cases:
 * 1. Empty string: Initialize with formatted call ID
 * 2. Call ID not in string: Append formatted call ID
 * 3. Call ID already present: Do nothing (avoid duplicates)
 *
 * Examples:
 * - existingCallIds = "", newCallId = "123" → existingCallIds = "123, "
 * - existingCallIds = "123, ", newCallId = "456" → existingCallIds = "123, 456,
 * "
 * - existingCallIds = "123, ", newCallId = "123" → existingCallIds = "123, "
 * (no change)
 *
 * @param existingCallIds Reference to existing call IDs string (modified
 * in-place)
 * @param newCallId New call ID to add
 *
 * @note Modifies existingCallIds in-place
 * @note Uses std::string::find to check for duplicates
 * @note Calls formatCallId() to format the new call ID
 */
void TicketSystem::addCallIdToExisting(std::string &existingCallIds,
                                       const std::string &newCallId) {
  if (existingCallIds.empty()) {
    // Initialize with first call ID
    existingCallIds = formatCallId(newCallId);
  } else if (existingCallIds.find(newCallId) == std::string::npos) {
    // Append only if not already present (avoid duplicates)
    existingCallIds += formatCallId(newCallId);
  }
  // If call ID already exists, do nothing
}

/**
 * @brief Remove a specific call ID from a comma-separated list
 *
 * Parses the comma-separated call IDs string and rebuilds it without the
 * specified call ID. Handles whitespace trimming and maintains proper
 * comma-separated format.
 *
 * Algorithm:
 * 1. Split string by comma delimiter
 * 2. Trim whitespace from each item
 * 3. Skip empty items and the call ID to remove
 * 4. Rebuild string with remaining call IDs
 *
 * Examples:
 * - existingCallIds = "123, 456, 789, ", callIdToRemove = "456"
 *   → existingCallIds = "123, 789, "
 * - existingCallIds = "123, ", callIdToRemove = "123"
 *   → existingCallIds = ""
 *
 * @param existingCallIds Reference to existing call IDs string (modified
 * in-place)
 * @param callIdToRemove Call ID to remove from the list
 *
 * @note Modifies existingCallIds in-place
 * @note Trims whitespace from each parsed item
 * @note Maintains comma-separated format with trailing ", "
 * @note Handles empty items gracefully (skips them)
 */
void TicketSystem::removeCallIdFromExisting(std::string &existingCallIds,
                                            const std::string &callIdToRemove) {
  std::string newCallIds = "";
  std::stringstream ss(existingCallIds);
  std::string item;
  bool found = false;

  // Parse comma-separated values and rebuild without the target call ID
  while (std::getline(ss, item, ',')) {
    // Trim whitespace from item
    size_t start = item.find_first_not_of(" \t");
    size_t end = item.find_last_not_of(" \t");
    if (start != std::string::npos && end != std::string::npos) {
      item = item.substr(start, end - start + 1);
    } else {
      item = ""; // If only whitespace, treat as empty
    }

    // Only add to new list if it's not empty and not the call ID we want to
    // remove
    if (!item.empty() && item != callIdToRemove) {
      newCallIds += item + ", ";
    } else if (item == callIdToRemove) {
      found = true;
    }
    // Skip empty items completely
  }

  existingCallIds = newCallIds;
}
