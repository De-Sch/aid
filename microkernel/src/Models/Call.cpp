/**
 * @file Call.cpp
 * @brief Implementation of Call data model constructors
 *
 * Provides three construction methods:
 * 1. 4-parameter constructor (for calls without user/agent)
 * 2. 5-parameter constructor (for calls with user/agent)
 * 3. JSON constructor (primary method - deserializes Asterisk AMI events)
 *
 * @see Call.h for detailed documentation
 */

#include "Models/Call.h"

namespace {
/**
 * @brief Helper function to safely extract string from JSON object
 *
 * If the field exists in the JSON object, assigns its value to the target
 * string. If the field is missing, leaves the target string unchanged (empty by
 * default).
 *
 * @param jsonData JSON object to search
 * @param fieldName Field name to look for
 * @param targetString Reference to string where value will be stored
 */
void extractStringField(const nlohmann::json &jsonData,
                        const std::string &fieldName,
                        std::string &targetString) {
  if (jsonData.contains(fieldName)) {
    targetString = jsonData[fieldName];
  }
}
} // namespace

/**
 * 4-parameter constructor implementation
 * Uses member initialization for clarity and efficiency
 */
Call::Call(const std::string &event, const std::string &callId,
           const std::string &phoneNumber, const std::string &dialedPhoneNumber)
    : event(event), callId(callId), phoneNumber(phoneNumber),
      dialedPhoneNumber(dialedPhoneNumber),
      user("") // Initialize user as empty string
{}

/**
 * 5-parameter constructor implementation
 * Preferred for calls with known agent/user
 */
Call::Call(const std::string &event, const std::string &callId,
           const std::string &phoneNumber, const std::string &dialedPhoneNumber,
           const std::string &user)
    : event(event), callId(callId), phoneNumber(phoneNumber),
      dialedPhoneNumber(dialedPhoneNumber), user(user) {}

/**
 * JSON constructor implementation
 *
 * Business Logic Note:
 * The Asterisk AMI sends different field names for user depending on event
 * type:
 * - "Accepted Call" events: use "user" field
 * - "Transfer Call" events: use "newuser" field (indicates new agent after
 * transfer) This constructor checks both fields, with "newuser" taking
 * precedence if both exist.
 */
Call::Call(const nlohmann::json &data) {
  // Extract standard fields from JSON
  extractStringField(data, "event", event);
  extractStringField(data, "callid", callId);
  extractStringField(data, "remote", phoneNumber);
  extractStringField(data, "dialed", dialedPhoneNumber);
  extractStringField(data, "user", user);

  // Special handling: Transfer events send "newuser" instead of "user"
  // If both exist, "newuser" takes precedence (last write wins)
  extractStringField(data, "newuser", user);
}

/**
 * Destructor implementation
 * No cleanup needed as all members are RAII-managed std::string objects
 */
Call::~Call() {}
