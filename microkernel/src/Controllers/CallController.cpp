/**
 * @file CallController.cpp
 * @brief Implementation of call event processing and ticket management
 *
 * This file implements the CallController class which handles the complete
 * lifecycle of phone calls in the AID system. It processes webhooks from the
 * phone system, coordinates with the address system plugin for caller lookup,
 * and manages tickets.
 *
 * Key Responsibilities:
 * - Parse JSON webhooks from phone system (3CX, Asterisk, etc.)
 * - Lookup caller information via AddressSystem plugin
 * - Create/update tickets in TicketSystem plugin
 * - Track call lifecycle: Ring → Accepted → Transfer → Hangup
 * - Record call history as comments in ticket descriptions
 * - Calculate call durations with DST awareness
 * - Handle multiple simultaneous calls per ticket
 *
 * Comment Format Examples:
 * - During call:  "john.doe: Call start: 2030-11-03 14:30:00 (abc123)"
 * - After hangup: "john.doe: Call start: 2030-11-03 14:30:00 Call End:
 * 2030-11-03 14:45:00 "Duration: 15min""
 *
 * Multi-Call Tracking:
 * - Ticket.callId stores comma-separated list: "abc123,def456,ghi789"
 * - Each callId gets its own comment line
 * - Multiple users can have concurrent calls on same ticket
 *
 * Error Handling:
 * - Non-critical errors (unknown user) return 1 and log warning
 * - Critical errors (ticket not found on accepted call) throw exception
 * - Duplicate prevention: checks for existing comments before adding
 */

#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "Constants.h"
#include "Controllers/CallController.h"
#include "Logger/Logger.h"
#include "Models/Call.h"
#include "Models/Ticket.h"
#include "Systems/AddressSystem.h"
#include "Systems/TicketSystem.h"

/**
 * @brief Constructor initializes controller with system dependencies
 */
CallController::CallController(TicketSystem &ticketSystem,
                               AddressSystem &addressSystem)
    : ticketSystem(ticketSystem), addressSystem(addressSystem) {
  // Dependency injection ensures TicketSystem and AddressSystem are available
}

/**
 * @brief Destructor cleans up resources
 */
CallController::~CallController() {
  // No dynamic resources to clean up
}

// ============================================================================
// Main Entry Points
// ============================================================================

/**
 * @brief Main entry point for call event processing
 *
 * Sends immediate "Accepted" HTTP response to prevent webhook timeouts,
 * then processes the call event. This ensures the phone system receives
 * acknowledgment quickly while ticket operations complete in background.
 *
 * @return Result code from processCallEvent()
 */
int CallController::run(std::istream &payload, std::string &urlParams) {
  int result = processCallEvent(payload, urlParams);

  // Send immediate HTTP response to phone system webhook
  std::cout << "Content-Type: text/plain\r\n\r\nAccepted" << std::endl;
  logging::Logger::info("HTTP response sent to phone system");

  return result;
}

/**
 * @brief Parse JSON webhook and route to appropriate event handler
 *
 * Workflow:
 * 1. Parse JSON payload into Call object
 * 2. Log call details (callId, event, phoneNumber, user)
 * 3. Route based on event type:
 *    - "Incoming Call" / "Outgoing Call" → handleRing()
 *    - "Accepted Call" → handleAcceptedCall()
 *    - "Transfer Call" → handleTransferCall()
 *    - "Hangup" → handleHangup()
 *
 * @param payload JSON webhook body from phone system
 * @param urlParams URL parameters (unused for call events)
 * @return 0 on success, error code on failure
 */
int CallController::processCallEvent(std::istream &payload,
                                     std::string &urlParams) {
  // Parse JSON webhook payload
  nlohmann::json data;
  payload.seekg(0);
  payload >> data;
  logging::Logger::info("ActionCall Data: " + data.dump(4));

  // Create Call object from JSON (validates required fields)
  Call call(data);

  // Log call details for debugging
  logging::Logger::info("Call: " + call.callId + " " + call.dialedPhoneNumber +
                        " " + call.event + " " + call.phoneNumber +
                        (call.user.empty() ? "" : " User: " + call.user));

  // Route to appropriate handler based on event type
  if (call.event == "Accepted Call")
    return handleAcceptedCall(call);
  else if (call.event == "Transfer Call")
    return handleTransferCall(call);
  else if (call.event == "Hangup")
    return handleHangup(call);
  else if (call.event == "Outgoing Call" || call.event == "Incoming Call")
    return handleRing(call);
  else {
    // Unknown event type - log error and return bad request
    logging::Logger::error("Unknown call event");
    std::cout << "Content-Type: text/plain\r\n\r\nUnknown call event"
              << std::endl;
    return Constants::HttpStatus::BAD_REQUEST;
  }
  return 0;
}

// ============================================================================
// Ticket Search Helper Methods
// ============================================================================

/**
 * @brief Find existing open ticket in any of the provided projects
 *
 * Iterates through project IDs and returns the first New or In Progress
 * Call ticket found. This enables finding the correct ticket when a contact
 * is associated with multiple projects.
 *
 * @param ids Vector of ticket system project IDs to search
 * @return Ticket pointer if found (caller owns memory), nullptr if not found
 */
Ticket *CallController::getExistingTicketByProjectIds(
    const std::vector<std::string> ids) const {
  for (auto &id : ids) {
    logging::Logger::info("Checking project ID: " + id);

    // Search for open Call ticket in this project
    if (auto ticket = ticketSystem.getLatestCallTicketInProject(id)) {
      logging::Logger::info("Found existing Call ticket ID: " + ticket->id +
                            " in project: " + id);
      return ticket;
    }

    logging::Logger::debug("No New/In Progress Call ticket found in project: " +
                           id);
  }
  return nullptr;
}

// ============================================================================
// Ring Event Handler and Helpers
// ============================================================================

/**
 * @brief Validate that user exists in ticket system
 *
 * For outgoing calls, the phone system provides the user field indicating
 * who initiated the call. This validation prevents ticket creation for
 * unknown users (e.g., external systems, test accounts).
 *
 * @param call Call object with optional user field
 * @return true if user valid or field empty, false if user doesn't exist
 */
bool CallController::validateUserExists(const Call &call) {
  if (!call.user.empty()) {
    if (!ticketSystem.checkIfUserExists(call.user)) {
      logging::Logger::info("User doesn't exist: " + call.user);
      return false;
    }
  }
  return true;
}

/**
 * @brief Find or create ticket for known contact (has project IDs)
 *
 * Known Contact Strategy:
 * 1. Search for existing open ticket in any of the contact's projects
 * 2. If found: Append callId to existing ticket (multi-call tracking)
 * 3. If not found: Create new ticket in first project with company/name title
 *
 * This strategy consolidates multiple calls from the same contact into
 * a single ticket during their open session.
 *
 * @param addressInfo Contact info from CardDAV (must have projectIds populated)
 * @param call Call object with callId
 * @return Ticket pointer (caller owns memory, never null)
 */
Ticket *CallController::findOrCreateTicketForKnownContact(
    const AddressSystem::addressInformation &addressInfo, const Call &call) {
  // Search for existing ticket in any of the contact's associated projects
  Ticket *ticket = getExistingTicketByProjectIds(addressInfo.projectIds);

  if (ticket) {
    // Existing ticket found - append new call ID to track multiple calls
    ticketSystem.addCallIdToExisting(ticket->callId, call.callId);
    logging::Logger::info("Updated call ID field: " + ticket->callId);
  } else {
    // No existing ticket - create new one in first project
    std::string projectId = addressInfo.projectIds[0];
    logging::Logger::info(
        "No existing Call ticket found, creating new ticket in project: " +
        projectId);
    ticket = ticketSystem.createNewTicket(addressInfo, call);
    ticket->title = addressInfo.companyName + " - " + addressInfo.name;
  }

  return ticket;
}

/**
 * @brief Find or create ticket for unknown number (no project IDs)
 *
 * Unknown Number Strategy:
 * 1. Try to find existing ticket by contact name (if CardDAV provided partial
 * info)
 * 2. Fallback: Try to find existing ticket by phone number
 * 3. If found: Append callId to existing ticket
 * 4. If not found: Create new ticket in default project with phone number as
 * title
 *
 * This handles callers who aren't in the CardDAV address books or whose
 * vCard entries don't have project associations.
 *
 * @param addressInfo Contact info from CardDAV (may be empty or partial)
 * @param call Call object with phoneNumber and callId
 * @return Ticket pointer (caller owns memory, never null)
 */
Ticket *CallController::findOrCreateTicketForUnknownNumber(
    const AddressSystem::addressInformation &addressInfo, const Call &call) {
  Ticket *ticket = nullptr;
  std::string title = call.phoneNumber;

  // Try to find by contact name first (if CardDAV returned any info)
  if (addressInfo.name.size()) {
    title = addressInfo.companyName + " - " + addressInfo.name;
    ticket = getExistingTicketByName(addressInfo.name);
  }

  // Fallback: search by phone number if name search failed
  if (!ticket) {
    ticket = getExistingTicketByName(call.phoneNumber);
  }

  if (ticket) {
    // Existing ticket found - append new call ID for multi-call tracking
    ticketSystem.addCallIdToExisting(ticket->callId, call.callId);
  } else {
    // No existing ticket - create new one in default location
    ticket = ticketSystem.createNewTicket(addressInfo, call);
    ticket->title = title;
  }

  return ticket;
}

/**
 * @brief Handle ring/incoming/outgoing call event
 *
 * Call Flow:
 * 1. Validate user exists (for outgoing calls) - skip if unknown user
 * 2. Lookup caller in address system
 * 3. Determine if caller is known (has projectIds) or unknown
 * 4. Find existing ticket or create new one based on contact type
 * 5. Set assignee if user provided (outgoing calls only)
 * 6. Save ticket with callId for tracking
 *
 * Duplicate Prevention:
 * - Known contacts: Search by projectIds (consolidates multi-project contacts)
 * - Unknown numbers: Search by name or phone in default location
 * - Multiple calls: Append callId to existing ticket
 *
 * @param call Call object with phoneNumber, callId, user (optional), event
 * @return 0 on success, 1 if user validation failed (non-critical)
 * @throws std::runtime_error if ticket creation/retrieval fails
 */
int CallController::handleRing(Call &call) {
  logging::Logger::info("Handling '" + call.event +
                        "' started for callId: " + call.callId);

  // Validate user exists (for outgoing calls with assigned user)
  if (!validateUserExists(call)) {
    return 1; // Non-critical error - skip this call
  }

  // Get caller information from address system (CardDAV lookup)
  AddressSystem::addressInformation addressInfo;
  addressSystem.getInformationByNumber(call, addressInfo);

  // Find or create ticket based on whether contact is known
  std::unique_ptr<Ticket> ticket;
  if (addressInfo.projectIds.size()) {
    // Known contact with project associations - search in their projects
    ticket.reset(findOrCreateTicketForKnownContact(addressInfo, call));
  } else {
    // Unknown number - search by name/phone in default location
    ticket.reset(findOrCreateTicketForUnknownNumber(addressInfo, call));
  }

  if (!ticket) {
    throw std::runtime_error("Failed to create or find ticket for handleRing");
  }

  // Set assignee if user is present (outgoing calls)
  if (!call.user.empty()) {
    logging::Logger::info("User found in call: " + call.user +
                          " - setting as assignee");
    if (!ticket->setTicketForAcceptedCall(call)) {
      return 1;
    }
  }

  // Save ticket with callId for event tracking
  logging::Logger::info("Saving ticket ID: " + ticket->id +
                        " for callId: " + call.callId);
  ticketSystem.saveTicket(ticket.get());
  logging::Logger::debug("handleRing completed successfully");
  return 0;
}

/**
 * @brief Find existing open ticket by name in default project
 *
 * Searches the default project (configUnknownNumberSaveLocation) for
 * New or In Progress tickets with matching name. Used for unknown
 * numbers to avoid creating duplicate tickets.
 *
 * @param name Contact name or phone number to search
 * @return Ticket pointer if found (caller owns), nullptr if not found
 */
Ticket *CallController::getExistingTicketByName(const std::string &name) const {
  // Search for existing tickets by name in default project with New or In
  // Progress status
  auto ticket = ticketSystem.getLatestTicketInProjectByName(
      ticketSystem.configUnknownNumberSaveLocation, name);
  if (!ticket)
    logging::Logger::info("No existing ticket found with name '" + name +
                          "' in default project");
  else
    logging::Logger::info("Found existing ticket ID: " + ticket->id +
                          " with name '" + name + "' in default project.");
  return ticket;
}

// ============================================================================
// Accepted Call Event Handler and Helpers
// ============================================================================

/**
 * @brief Handle accepted call event (call connected by user)
 *
 * Call Flow:
 * 1. Validate user exists (if provided in webhook)
 * 2. Find ticket by callId (MUST exist - created in handleRing)
 * 3. Set call.user from ticket assignee if webhook didn't provide user
 * 4. Update ticket status to "In Progress" (if not already closed)
 * 5. Record call start timestamp (first call only)
 * 6. Add comment with username, timestamp, and callId
 * 7. Save ticket
 *
 * Comment Format: "username: Call start: YYYY-MM-DD HH:MM:SS (callId)"
 *
 * Multi-Call Handling:
 * - callStartTimestamp is set only on first call (field empty)
 * - Each call gets its own comment line with unique callId
 * - Duplicate prevention: Checks if comment already exists before adding
 *
 * @param call Call object with callId, user (optional)
 * @return 0 on success, 1 if user validation failed or ticket not found
 */
int CallController::handleAcceptedCall(Call &call) {
  logging::Logger::info("Handling 'Accepted Call' started for callId: " +
                        call.callId);

  // Check if user exists in ticketsystem to avoid tickets from unknown user
  if (!call.user.empty()) {
    if (!ticketSystem.checkIfUserExists(call.user)) {
      logging::Logger::info("User doesn't exist: " + call.user);
      return 1;
    }
  }

  // For Accepted Call, ticket MUST exist (created in handleRing)
  std::unique_ptr<Ticket> ticket(ticketSystem.getTicketByCallId(call.callId));

  if (!ticket) {
    logging::Logger::error(
        "CRITICAL: No ticket found for accepted call with callId: " +
        call.callId);
    return 1;
  }
  logging::Logger::info("Found ticket ID: " + ticket->id + " with status: " +
                        ticket->status + " for callId: " + call.callId);

  // If webhook didn't provide user, use ticket assignee
  if (call.user.empty()) {
    if (!ticket->userInformation.empty()) {
      call.user = ticket->userInformation;
      logging::Logger::info("User was empty, replaced with current assignee: " +
                            call.user);
    }
    if (call.user.empty()) {
      logging::Logger::info(
          "User was empty but no assignee found in ticket, keeping as is");
    }
  }

  // Only change status to In Progress if it's not already closed
  if (ticket->status != Constants::TicketStatus::CLOSED) {
    ticket->status = ticket->api.configStatusInProgress;
    logging::Logger::info("Set ticket status to In Progress for callId: " +
                          call.callId);
  } else {
    logging::Logger::error(
        "Cannot change status from Closed to In Progress for ticket ID: " +
        ticket->id);
  }

  logging::Logger::debug("handlingCallConnected: Ticket status '" +
                         ticket->status + "', expected InProgress: '" +
                         ticketSystem.configStatusInProgress + "'");
  ticket->setTicketForAcceptedCall(call);

  // Save call start timestamp only if this is the first call (field is empty)
  std::string currentTimestamp = getCurrentTimestamp();

  if (ticket->callStartTimestamp.empty()) {
    ticket->callStartTimestamp = currentTimestamp;
    logging::Logger::info("Set call start timestamp (first call): " +
                          ticket->callStartTimestamp);
  } else {
    logging::Logger::info(
        "Call start timestamp already exists (not first call): " +
        ticket->callStartTimestamp + ", current: " + currentTimestamp);
  }

  // Add new comment for accepted call with callId tracking (using current
  // timestamp for comment)
  if (!call.user.empty()) {
    // Log current description before modification for debugging
    logging::Logger::info(
        "Current ticket description before adding comment: '" +
        ticket->description + "'");

    // Check for duplicates and add comment if new
    if (!isCallAlreadyRecorded(ticket->description, call.user, call.callId)) {
      std::string commentLine =
          formatCallStartComment(call.user, currentTimestamp, call.callId);

      // Safely append the new comment (newline separator if description
      // non-empty)
      if (!ticket->description.empty()) {
        ticket->description += "\n" + commentLine;
      } else {
        ticket->description = commentLine;
      }
      logging::Logger::info("Added new comment line for callId " + call.callId +
                            ": " + commentLine);
    } else {
      logging::Logger::info("Comment for user " + call.user + " with callId " +
                            call.callId +
                            " already exists in ticket, skipping duplicate");
    }

    // Log final description after modification for debugging
    logging::Logger::info("Final ticket description after adding comment: '" +
                          ticket->description + "'");
  }

  logging::Logger::info("Call accepted - Ticket ID: " + ticket->id +
                        " start time: " + ticket->callStartTimestamp);

  // Save updated ticket to ticket system
  ticketSystem.saveTicket(ticket.get());

  return 0;
}

/**
 * @brief Get current timestamp in format "YYYY-MM-DD HH:MM:SS"
 *
 * Uses localtime for timezone-aware timestamps. Used for recording call
 * start times in both the custom field and comment lines.
 *
 * @return Current timestamp string
 */
std::string CallController::getCurrentTimestamp() {
  auto now = std::chrono::system_clock::now();
  auto time_t = std::chrono::system_clock::to_time_t(now);
  std::stringstream ss;
  ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
  return ss.str();
}

/**
 * @brief Check if call already recorded in description (duplicate prevention)
 *
 * Scans ticket description for existing comment with matching user and callId.
 * This prevents duplicate comments when the same call event is received
 * multiple times (e.g., webhook retries, system restarts during call).
 *
 * Search Strategy:
 * 1. Find all lines starting with "username: Call start:"
 * 2. Check if any contain "(callId)"
 * 3. Return true if match found
 *
 * @param description Ticket description with zero or more comment lines
 * @param user Username to search for
 * @param callId CallId to search for
 * @return true if duplicate found, false if new comment should be added
 */
bool CallController::isCallAlreadyRecorded(const std::string &description,
                                           const std::string &user,
                                           const std::string &callId) {
  // Build search patterns
  std::string userCallPattern =
      user + Constants::CommentFormat::CALL_START_PREFIX;
  std::string callIdPattern = "(" + callId + ")";

  // Search for user's call comments
  size_t userPos = description.find(userCallPattern);

  // Scan all lines with this user's name to check for duplicate callId
  while (userPos != std::string::npos) {
    // Find the end of this line
    size_t lineEnd = description.find('\n', userPos);
    if (lineEnd == std::string::npos)
      lineEnd = description.length();

    // Check if this line contains our callId (prevents double-adding same call)
    std::string line = description.substr(userPos, lineEnd - userPos);
    if (line.find(callIdPattern) != std::string::npos) {
      return true; // Duplicate found
    }

    // Search for next occurrence of this user
    userPos = description.find(userCallPattern, lineEnd);
  }

  return false; // No duplicate found
}

/**
 * @brief Format call start comment line
 *
 * Creates comment in standard format for new accepted calls.
 * Format: "username: Call start: YYYY-MM-DD HH:MM:SS (callId)"
 *
 * The callId in parentheses enables later lookup during transfer/hangup events.
 *
 * @param user Username handling the call
 * @param timestamp Call start timestamp
 * @param callId Unique call identifier for tracking
 * @return Formatted comment line
 */
std::string CallController::formatCallStartComment(const std::string &user,
                                                   const std::string &timestamp,
                                                   const std::string &callId) {
  return user + ": Call start: " + timestamp + " (" + callId + ")";
}

// ============================================================================
// Transfer Call Event Handler and Helpers
// ============================================================================

/**
 * @brief Update ticket status to "In Progress" for transferred call
 *
 * Only updates status if ticket is not already closed (prevents reopening
 * closed tickets when transfers occur after manual closure).
 *
 * @param ticket Ticket to update (non-null)
 * @return true if status changed, false if already closed
 */
bool CallController::updateTicketStatusForTransfer(Ticket *ticket) {
  if (ticket->status != Constants::TicketStatus::CLOSED) {
    ticket->status = ticket->api.configStatusInProgress;
    logging::Logger::info("Set ticket status to In Progress for ticket ID: " +
                          ticket->id);
    return true;
  } else {
    logging::Logger::error(
        "Cannot change status from Closed to In Progress for ticket ID: " +
        ticket->id);
    return false;
  }
}

/**
 * @brief Update username in comment line for transferred call
 *
 * When a call is transferred, the comment line needs to reflect the new
 * assignee. This finds the line containing "(callId)" and replaces the username
 * prefix while preserving the timestamp and callId.
 *
 * Example:
 * Before: "john.doe: Call start: 2030-11-03 14:30:00 (abc123)"
 * After:  "jane.smith: Call start: 2030-11-03 14:30:00 (abc123)"
 *
 * @param description Ticket description (modified in place)
 * @param callId CallId to identify which comment line to update
 * @param newUsername New assignee username
 * @return true on success, false if line not found or parse error
 */
bool CallController::updateCommentLineUsername(std::string &description,
                                               const std::string &callId,
                                               const std::string &newUsername) {
  // Find line containing this callId (pattern: "(callId)")
  std::string searchPattern = "(" + callId + ")";
  size_t pos = description.find(searchPattern);
  if (pos == std::string::npos) {
    logging::Logger::error("Could not find comment line with callId: " +
                           callId);
    return false;
  }

  // Locate line boundaries
  size_t lineStart = description.rfind('\n', pos);
  lineStart = (lineStart == std::string::npos) ? 0 : lineStart + 1;

  size_t lineEnd = description.find('\n', pos);
  lineEnd = (lineEnd == std::string::npos) ? description.length() : lineEnd;

  // Extract and parse the line (format: "Username: Call start: ...")
  std::string originalLine = description.substr(lineStart, lineEnd - lineStart);
  logging::Logger::info("Original comment line: " + originalLine);

  size_t firstColonPos = originalLine.find(':');
  if (firstColonPos == std::string::npos) {
    logging::Logger::error("Could not find colon in comment line for callId: " +
                           callId);
    return false;
  }

  // Replace username prefix, keep everything after the first colon
  std::string restOfLine = originalLine.substr(firstColonPos);
  std::string newLine = newUsername + restOfLine;
  description.replace(lineStart, lineEnd - lineStart, newLine);

  logging::Logger::info("Updated comment line for transfer callId " + callId +
                        ": " + newLine);
  return true;
}

/**
 * @brief Handle call transfer event (call reassigned to different user)
 *
 * Call Flow:
 * 1. Find ticket by callId (may match multiple callIds in list)
 * 2. Update ticket status to "In Progress" (if not closed)
 * 3. Update assignee to new user
 * 4. Find and update username in comment line for this callId
 * 5. Save ticket
 *
 * The key operation is updating the comment line username while preserving
 * the original timestamp, which tracks when the call actually started (not
 * when it was transferred).
 *
 * @param call Call object with callId, user (new assignee)
 * @return 0 on success, 1 if ticket not found or comment update failed
 */
int CallController::handleTransferCall(Call &call) {
  logging::Logger::info("Handling 'Transfer Call' started for callId: " +
                        call.callId);

  // Retrieve existing ticket containing this callId
  std::unique_ptr<Ticket> ticket(
      ticketSystem.getTicketByCallIdContains(call.callId));
  if (!ticket) {
    logging::Logger::error(
        "CRITICAL: No ticket found for transfer call with callId: " +
        call.callId);
    return 1;
  }
  logging::Logger::info("Found ticket ID: " + ticket->id +
                        " with status: " + ticket->status);

  // Update ticket status to In Progress (if not already closed)
  updateTicketStatusForTransfer(ticket.get());

  // Update the assignee to the new user receiving the transfer
  ticket->setTicketForAcceptedCall(call);

  // Update the username in the existing comment line for this callId
  if (!updateCommentLineUsername(ticket->description, call.callId, call.user)) {
    return 1;
  }

  logging::Logger::info("Call transferred - Ticket ID: " + ticket->id +
                        " to user: " + call.user);

  // Save updated ticket to ticket system
  ticketSystem.saveTicket(ticket.get());
  return 0;
}

// ============================================================================
// Hangup Event Handler and Helpers
// ============================================================================

/**
 * @brief Find comment line containing specific callId
 *
 * Searches for pattern "(callId)" in description and returns the start
 * position of that line. Used during hangup to locate the comment line
 * that needs to be completed with end time and duration.
 *
 * @param description Ticket description with multiple comment lines
 * @param callId Unique call identifier to search for
 * @return Line start position, or string::npos if not found
 */
size_t CallController::findCommentLineByCallId(const std::string &description,
                                               const std::string &callId) {
  std::string searchPattern = "(" + callId + ")";
  size_t pos = description.find(searchPattern);

  if (pos == std::string::npos) {
    return std::string::npos;
  }

  // Locate line start boundary
  size_t lineStart = description.rfind('\n', pos);
  if (lineStart == std::string::npos) {
    return 0; // First line
  }
  return lineStart + 1; // Skip the newline character
}

/**
 * @brief Extract timestamp from comment line
 *
 * Parses comment format: "Username: Call start: YYYY-MM-DD HH:MM:SS (callId)"
 * to extract the timestamp portion between "Call start:" and " (".
 *
 * @param commentLine Single comment line from ticket description
 * @return Timestamp string "YYYY-MM-DD HH:MM:SS", or empty on parse failure
 */
std::string
CallController::extractTimestampFromComment(const std::string &commentLine) {
  std::string startPattern = Constants::CommentFormat::CALL_START_PREFIX;
  size_t startPos = commentLine.find(startPattern);

  if (startPos == std::string::npos) {
    return "";
  }

  startPos += startPattern.length();
  size_t endPos = commentLine.find(" (", startPos);

  if (endPos == std::string::npos) {
    return "";
  }

  return commentLine.substr(startPos, endPos - startPos);
}

/**
 * @brief Calculate call duration in minutes with DST awareness
 *
 * Uses std::mktime with tm_isdst=-1 to handle daylight saving time transitions
 * correctly. This ensures accurate duration calculation even when calls span
 * DST boundaries (e.g., during spring/fall time changes).
 *
 * DST Handling:
 * - tm_isdst=-1 tells mktime to determine DST automatically based on timestamp
 * - Prevents 1-hour errors when calls occur during DST transitions
 *
 * @param startTimestamp Start time "YYYY-MM-DD HH:MM:SS"
 * @param endTimestamp End time "YYYY-MM-DD HH:MM:SS"
 * @return Duration in minutes, or -1 on parse error
 */
int CallController::calculateDurationMinutes(const std::string &startTimestamp,
                                             const std::string &endTimestamp) {
  if (startTimestamp.empty() || endTimestamp.empty()) {
    return -1;
  }

  // Parse start timestamp
  std::tm startTm = {};
  std::istringstream startSs(startTimestamp);
  startSs >> std::get_time(&startTm, "%Y-%m-%d %H:%M:%S");

  if (startSs.fail()) {
    logging::Logger::error("Failed to parse start timestamp: " +
                           startTimestamp);
    return -1;
  }

  // Parse end timestamp
  std::tm endTm = {};
  std::istringstream endSs(endTimestamp);
  endSs >> std::get_time(&endTm, "%Y-%m-%d %H:%M:%S");

  if (endSs.fail()) {
    logging::Logger::error("Failed to parse end timestamp: " + endTimestamp);
    return -1;
  }

  // Use local time with DST auto-determination to avoid timezone issues
  startTm.tm_isdst = -1; // Let mktime determine daylight saving time
  endTm.tm_isdst = -1;

  auto startTimePoint =
      std::chrono::system_clock::from_time_t(std::mktime(&startTm));
  auto endTimePoint =
      std::chrono::system_clock::from_time_t(std::mktime(&endTm));

  // Calculate duration in minutes
  auto duration = std::chrono::duration_cast<std::chrono::minutes>(
      endTimePoint - startTimePoint);
  return duration.count();
}

/**
 * @brief Format completed comment line with duration
 *
 * Creates final comment format with start time, end time, and duration.
 * Format: "username: Call start: START Call End: END "Duration: Xmin""
 *
 * The duration is quoted for visual emphasis in the ticket description.
 *
 * @param username User who handled the call
 * @param startTime Call start timestamp
 * @param endTime Call end timestamp
 * @param duration Duration string (e.g., "15")
 * @return Formatted completed comment line
 */
std::string CallController::formatCompletedComment(
    const std::string &username, const std::string &startTime,
    const std::string &endTime, const std::string &duration) {
  return username + ": Call start: " + startTime + " Call End: " + endTime +
         " \"Duration: " + duration + "min\"";
}

/**
 * @brief Remove callId from comma-separated list
 *
 * Wrapper for ticketSystem.removeCallIdFromExisting() which handles the
 * actual parsing and removal logic. Used to clean up the callId tracking
 * field after call completion.
 *
 * @param callIdList Comma-separated callId list (modified in place)
 * @param callIdToRemove CallId to remove from list
 * @return Updated callId list
 */
std::string
CallController::removeCallIdFromList(std::string &callIdList,
                                     const std::string &callIdToRemove) {
  // Delegate to TicketSystem which modifies callIdList in place
  ticketSystem.removeCallIdFromExisting(callIdList, callIdToRemove);
  return callIdList;
}

/**
 * @brief Handle call hangup event (call ended)
 *
 * This is the most complex event handler, responsible for completing the call
 * record by calculating duration and finalizing the comment line.
 *
 * Call Flow:
 * 1. Find ticket by callId (MUST exist)
 * 2. Generate and save call end timestamp
 * 3. Find comment line containing this callId
 * 4. Extract username and start time from comment
 * 5. Calculate duration (DST-aware)
 * 6. Format completed comment with start, end, duration
 * 7. Replace incomplete comment with completed version
 * 8. Remove callId from tracking field
 * 9. Save ticket
 *
 * Comment Transformation:
 * Before: "john.doe: Call start: 2030-11-03 14:30:00 (abc123)"
 * After:  "john.doe: Call start: 2030-11-03 14:30:00 Call End: 2030-11-03
 * 14:45:00 "Duration: 15min""
 *
 * Edge Cases:
 * - Comment not found: Only remove callId from tracking field (shouldn't
 * happen)
 * - Start time extraction fails: Use default duration (15 minutes)
 * - Duration calculation fails: Use default duration
 *
 * Duration Calculation:
 * - Uses DST-aware calculation (tm_isdst=-1)
 * - No rounding - uses actual minute duration
 * - Falls back to default (15 min) on any error
 *
 * @param call Call object with callId
 * @return 0 on success
 * @throws std::runtime_error if ticket not found (critical error)
 */
int CallController::handleHangup(Call &call) {
  logging::Logger::info("Handling 'Hang Up' started for callId: " +
                        call.callId);

  // Retrieve ticket containing this callId
  std::unique_ptr<Ticket> ticket(
      ticketSystem.getTicketByCallIdContains(call.callId));
  if (!ticket) {
    throw std::runtime_error(
        "CRITICAL: No ticket found for hangup call with callId: " +
        call.callId);
  }

  // Generate and save end timestamp to Call End field
  auto now = std::chrono::system_clock::now();
  auto endTime_t = std::chrono::system_clock::to_time_t(now);
  std::stringstream endSs;
  endSs << std::put_time(std::localtime(&endTime_t), "%Y-%m-%d %H:%M:%S");
  ticket->callEndTimestamp = endSs.str();

  // Default duration to 15 minutes if calculation fails
  std::string calculatedDuration =
      std::to_string(Constants::CallDuration::DEFAULT_MINUTES);

  // Find the comment line containing this callId
  size_t lineStart = findCommentLineByCallId(ticket->description, call.callId);

  if (lineStart != std::string::npos) {
    // Comment with callId found - extract details and complete the comment
    logging::Logger::info("Found comment with callId: " + call.callId +
                          " - processing completion");

    // Locate line end boundary
    size_t lineEnd = ticket->description.find('\n', lineStart);
    if (lineEnd == std::string::npos) {
      lineEnd = ticket->description.length(); // Last line
    }

    // Extract the original comment line
    std::string originalLine =
        ticket->description.substr(lineStart, lineEnd - lineStart);

    // Parse username from line format: "Username: Call start: ..."
    std::string extractedUser = "";
    size_t colonPos =
        originalLine.find(Constants::CommentFormat::CALL_START_PATTERN);
    if (colonPos != std::string::npos) {
      extractedUser = originalLine.substr(0, colonPos);
    }

    // Extract timestamp from comment line
    std::string extractedStartTime = extractTimestampFromComment(originalLine);

    // Calculate duration if we successfully extracted the start time
    if (!extractedStartTime.empty()) {
      // Calculate actual call duration in minutes using DST-aware calculation
      int durationMinutes = calculateDurationMinutes(extractedStartTime,
                                                     ticket->callEndTimestamp);

      if (durationMinutes >= 0) {
        // Use calculated duration (actual minutes, no rounding)
        calculatedDuration = std::to_string(durationMinutes);

        logging::Logger::info(
            "Call duration: " + std::to_string(durationMinutes) +
            " minutes (from comment start: " + extractedStartTime +
            "), using: " + calculatedDuration + " minutes");
      } else {
        logging::Logger::error("Duration calculation failed, using default: " +
                               calculatedDuration + " minutes");
      }

      // Build final comment with start, end, and duration using helper
      std::string finalComment =
          formatCompletedComment(extractedUser, extractedStartTime,
                                 ticket->callEndTimestamp, calculatedDuration);

      // Replace incomplete comment with final version
      ticket->description.replace(lineStart, lineEnd - lineStart, finalComment);
      logging::Logger::info("Successfully finished comment for callId " +
                            call.callId + " with: " + finalComment);
    } else {
      logging::Logger::error("Failed to extract start time from comment line, "
                             "using default duration");
    }

    // Remove callId from tracking field now that call is complete
    std::string previousCallIds = ticket->callId;
    removeCallIdFromList(ticket->callId, call.callId);
    logging::Logger::info(
        "Removed callId from custom field after comment completion: '" +
        ticket->callId + "' (was: '" + previousCallIds + "')");

  } else {
    // No comment found - just remove callId from tracking field (edge case,
    // shouldn't happen)
    logging::Logger::info("No comment found with callId: " + call.callId +
                          " - only removing from custom field");

    std::string previousCallIds = ticket->callId;
    removeCallIdFromList(ticket->callId, call.callId);
    logging::Logger::info(
        "Removed callId from custom field (no comment found): '" +
        ticket->callId + "' (was: '" + previousCallIds + "')");
  }

  logging::Logger::info("Call ended - Ticket ID: " + ticket->id +
                        " duration: " + calculatedDuration + " minutes");

  // Save the updated ticket to ticket system
  ticketSystem.saveTicket(ticket.get());
  return 0;
}
