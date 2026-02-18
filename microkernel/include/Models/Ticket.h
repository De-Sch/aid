/**
 * @file Ticket.h
 * @brief Abstract base class for ticket/work package data models
 *
 * This is an abstract base class that defines the interface for ticket
 * management in various backend systems. Tickets represent work packages or
 * support tickets created from phone call events.
 *
 * Design Pattern: Abstract Factory + Strategy
 * - Concrete implementations are loaded as plugins (examples: OpenProject,
 * Jira, etc.)
 * - Plugin loading uses dynamic libraries (.so files) with creator functions
 * - Each implementation handles system-specific API calls and data formats
 *
 * Lifecycle:
 * 1. Ring event   → Create/find ticket, populate caller info
 * 2. Accept event → Set start time, assign to agent, update status
 * 3. Transfer     → Update assignee
 * 4. Hangup       → Calculate duration, finalize, optionally close
 *
 * @dependencies TicketSystem (abstract interface), Call (event data)
 * @implementations See plugin implementations in lib/ folder
 *
 */

#pragma once

#include "Systems/TicketSystem.h"
#include "json.hpp"
#include <iostream>
#include <string>

// Forward declaration to avoid circular dependency
struct Call;

/**
 * @struct Ticket
 * @brief Abstract base class representing a support ticket/work package
 *
 * Core Responsibilities:
 * - Store ticket metadata (ID, title, description, timestamps, etc.)
 * - Provide interface for API response deserialization
 * - Calculate call duration from start/end timestamps
 * - Update ticket state for accepted calls
 *
 * Plugin Architecture:
 * - Concrete implementations are loaded via dynamic libraries
 * - Creator function signature: ticketDllCreate(config, api)
 * - Must implement: toTicketFromApiResponse(), getCallLength(),
 * setTicketForAcceptedCall()
 *
 * Data Members:
 * - Required: id, callId, title, status, description, timestamps
 * - Optional: call start/end times, call length, project ID
 */
struct Ticket {
  /**
   * @brief Constructs Ticket with TicketSystem reference (minimal constructor)
   * @param api Reference to TicketSystem for API operations
   */
  Ticket(const TicketSystem &api);

  /**
   * @brief Constructs Ticket from configuration (primary constructor)
   *
   * Loads plugin configuration (library path, system name) and initializes
   * ticket with system-specific settings.
   *
   * @param config JSON configuration object with plugin settings
   * @param api Reference to TicketSystem for API operations
   */
  Ticket(nlohmann::json &config, const TicketSystem &api);

  /**
   * @brief Virtual destructor for proper cleanup of derived classes
   */
  virtual ~Ticket() = default;

  // ==================== PUBLIC DATA MEMBERS ====================
  // Note: Public members for struct semantics (data aggregation)

  const TicketSystem
      &api; ///< Reference to TicketSystem API for backend operations

  // Required ticket fields (present in all implementations)
  std::string id; ///< Unique ticket/work package ID (system-specific format)
  std::string callId; ///< Asterisk call ID linking this ticket to phone call
  std::string title;  ///< Ticket subject/title (usually includes caller info)
  std::string userInformation; ///< Assigned agent/user (system-specific format)
  std::string callerNumber;    ///< Phone number of caller (E.164 format)
  std::string calledNumber;    ///< Dialed number (DID/extension)
  std::string status; ///< Ticket status (e.g., "new", "in-progress", "closed")
  std::string description; ///< Ticket description/body (markdown or plain text)
  std::string ticketLocationId; ///< Project/location ID where ticket resides
  std::string createdAt;        ///< ISO 8601 timestamp of ticket creation
  std::string updatedAt;        ///< ISO 8601 timestamp of last update
  std::string lockVersion;      ///< Optimistic locking version (for concurrent
                                ///< update prevention)

  // Optional call-related fields
  std::string callStartTimestamp = ""; ///< ISO 8601 timestamp when call was
                                       ///< accepted (empty if not yet accepted)
  std::string callEndTimestamp =
      ""; ///< ISO 8601 timestamp when call ended (empty if call ongoing)
  std::string
      callLength; ///< Calculated call duration (format: HH:MM:SS or seconds)
  std::string projectId; ///< Project ID associated with caller (for routing)

  // ==================== PURE VIRTUAL METHODS (must be implemented)
  // ====================

  /**
   * @brief Deserialize ticket from API response stream
   *
   * Parses API response (usually JSON) and populates this ticket's fields.
   * Implementation is system-specific.
   *
   * @param response Input stream containing API response body
   * @return true if parsing succeeded, false on error
   */
  virtual bool toTicketFromApiResponse(std::istream &response) = 0;

  /**
   * @brief Calculate call duration from start/end timestamps
   *
   * Computes callLength field based on callStartTimestamp and callEndTimestamp.
   * Format and precision are implementation-specific.
   *
   * @return true if calculation succeeded, false if timestamps invalid/missing
   */
  virtual bool getCallLength() = 0;

  /**
   * @brief Update ticket for accepted call event
   *
   * Sets call start timestamp, updates assignee, changes status to
   * "in-progress". This method is called when an agent accepts an incoming
   * call.
   *
   * @param call Call event data containing user (agent name)
   * @return true if update succeeded, false on error
   */
  virtual bool setTicketForAcceptedCall(Call &call) = 0;

protected:
  // ==================== PROTECTED HELPER METHODS ====================

  /**
   * @brief Safely extract configuration value from JSON with default fallback
   *
   * Template method to read config values with type safety. If field is
   * missing, sets default value in config and marks error flag.
   *
   * @tparam T Type of configuration value (string, int, bool, etc.)
   * @param config JSON configuration object
   * @param param Field name to extract
   * @param defaultVal Default value if field missing
   * @param hasError Output flag set to true if field was missing
   * @return Extracted value or default if missing
   */
  template <typename T>
  T getConfigValue(nlohmann::json &config, const char *param,
                   const T &defaultVal, bool &hasError);

  /**
   * @brief Get current date/time in system-specific format
   *
   * Pure virtual method for timestamp generation. Format depends on
   * backend API requirements.
   *
   * @return Current timestamp as string
   */
  virtual std::string getDateTimeNow() = 0;
};

/**
 * @typedef ticketDllCreate
 * @brief Function signature for dynamic library ticket creator
 *
 * All ticket plugins must export a "create" function with this signature.
 * This function is loaded via dlsym() and called to instantiate concrete Ticket
 * subclasses.
 *
 * @param config Plugin configuration (libPath, system settings)
 * @param api TicketSystem reference for API operations
 * @return Pointer to concrete Ticket implementation (caller owns memory)
 *
 * @example In ticket system plugin:
 * extern "C" Ticket* create(nlohmann::json& config, TicketSystem& api) {
 *     return new TicketSystemWorkPackage(config, api);
 * }
 */
typedef Ticket *(ticketDllCreate)(nlohmann::json &config, TicketSystem &api);
