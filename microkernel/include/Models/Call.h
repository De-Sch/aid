/**
 * @file Call.h
 * @brief Data model representing a phone call event from Asterisk PBX
 *
 * This structure encapsulates call event data received from the Asterisk AMI
 * (Asterisk Manager Interface) via the Python bridge (telixCallHandler).
 *
 * Call events include:
 * - "Incoming Call": Phone ringing, not yet answered
 * - "Accepted Call": Agent picked up the call
 * - "Transfer Call": Call transferred to different agent
 * - "Outgoing Call": Agent initiated outbound call
 * - "Hangup": Call ended
 *
 * @dependencies nlohmann::json for JSON deserialization
 * @callers CallController (processes events and manages ticket lifecycle)
 *
 */

#pragma once

#include "json.hpp"
#include <string>

/**
 * @struct Call
 * @brief Represents a single phone call event with associated metadata
 *
 * Design Notes:
 * - Plain struct (not class) for simple data aggregation
 * - All members are public for direct access by controllers
 * - Immutable after construction (callers should not modify)
 * - Supports JSON deserialization from Asterisk AMI events
 *
 * JSON Mapping:
 * - event        "event" field
 * - callId       "callid" field (unique Asterisk call identifier)
 * - phoneNumber  "remote" field (caller's phone number)
 * - dialedPhoneNumber  "dialed" field (dialed/DID number)
 * - user         "user" field (agent name) OR "newuser" field (transfer
 * events)
 */
struct Call {
  /**
   * @brief Constructs Call from individual string parameters (4-parameter
   * version)
   * @param event Event type (e.g., "Incoming Call", "Hangup")
   * @param callId Unique Asterisk call identifier
   * @param phoneNumber Remote party's phone number (E.164 format recommended)
   * @param dialedPhoneNumber Dialed number (DID/extension)
   */
  Call(const std::string &event, const std::string &callId,
       const std::string &phoneNumber, const std::string &dialedPhoneNumber);

  /**
   * @brief Constructs Call from individual string parameters (5-parameter
   * version with user)
   * @param event Event type
   * @param callId Unique Asterisk call identifier
   * @param phoneNumber Remote party's phone number
   * @param dialedPhoneNumber Dialed number
   * @param user Agent name who handled/is handling the call
   */
  Call(const std::string &event, const std::string &callId,
       const std::string &phoneNumber, const std::string &dialedPhoneNumber,
       const std::string &user);

  /**
   * @brief Constructs Call from JSON payload (primary deserialization method)
   *
   * Parses JSON received from Asterisk AMI bridge. Handles missing fields
   * gracefully by leaving corresponding member as empty string.
   *
   * Special handling:
   * - Checks both "user" and "newuser" fields (Transfer Call events use
   * "newuser")
   *
   * @param data JSON object from Asterisk AMI event
   *
   * @example
   * nlohmann::json eventData = nlohmann::json::parse(requestBody);
   * Call call(eventData);
   */
  Call(const nlohmann::json &data);

  /**
   * @brief Destructor (currently no-op as all members are RAII-managed)
   */
  ~Call();

  // Call event data members (all public for struct semantics)
  std::string event; ///< Event type: "Incoming Call", "Accepted Call",
                     ///< "Transfer Call", "Outgoing Call", "Hangup"
  std::string
      callId; ///< Unique Asterisk call identifier (persistent across transfers)
  std::string phoneNumber; ///< Remote party phone number (caller for incoming,
                           ///< callee for outgoing)
  std::string
      dialedPhoneNumber; ///< Dialed number (DID/extension that was called)
  std::string
      user; ///< Agent name handling the call (empty for Incoming/Hangup events)
};
