/**
 * @file CallController.h
 * @brief Controller for handling phone system call events
 *
 * This controller processes webhook events from the phone system, managing the
 * complete call lifecycle from ring to hangup. It coordinates between the phone
 * system, ticket system plugin, and address system plugin to automatically
 * create and update tickets for incoming and outgoing calls.
 *
 * Call Lifecycle:
 * 1. Ring Event (Incoming/Outgoing Call) - Creates or finds ticket, sets
 * assignee
 * 2. Accepted Call - Updates status to "In Progress", records call start time
 * 3. Transfer Call - Changes assignee, updates comment username
 * 4. Hangup - Records end time, calculates duration, completes comment
 *
 * Key Features:
 * - Automatic ticket creation based on caller lookup
 * - Call start/end timestamp tracking
 * - Duration calculation with DST awareness
 * - Multi-call tracking (multiple simultaneous calls per ticket)
 * - Comment-based call history in ticket description
 */

#pragma once
#include "Controller.h"
#include "Systems/AddressSystem.h"
#include "json.hpp"
#include <iostream>

using namespace std;

struct Call;
class TicketSystem;
struct Ticket;

/**
 * @class CallController
 * @brief Handles phone system webhook events and manages call-related tickets
 *
 * This controller is the heart of the call management system. It processes JSON
 * webhooks from the phone system and orchestrates ticket creation, updates, and
 * call tracking across the entire call lifecycle.
 *
 * Architecture:
 * - Receives JSON webhooks from phone system (e.g., 3CX, Asterisk)
 * - Looks up caller information via AddressSystem plugin
 * - Creates/updates tickets via TicketSystem plugin
 * - Tracks multiple simultaneous calls using callId field
 * - Records call history as comments in ticket description
 *
 * Call Event Flow:
 * 1. Ring/Incoming/Outgoing Call:
 *    - Lookup caller in CardDAV address books
 *    - Find existing ticket or create new one
 *    - Set assignee if provided (outgoing calls)
 *    - Store callId for tracking
 *
 * 2. Accepted Call:
 *    - Find ticket by callId
 *    - Set status to "In Progress"
 *    - Record call start timestamp
 *    - Add comment: "Username: Call start: TIMESTAMP (callId)"
 *
 * 3. Transfer Call:
 *    - Find ticket by callId
 *    - Update assignee to new user
 *    - Update username in comment for this callId
 *
 * 4. Hangup:
 *    - Find ticket by callId
 *    - Record call end timestamp
 *    - Calculate duration (DST-aware)
 *    - Complete comment: "Username: Call start: START Call End: END "Duration:
 * Xmin""
 *    - Remove callId from tracking field
 *
 * Comment Format:
 * - Incomplete (during call): "Username: Call start: 2030-11-03 14:30:00
 * (abc123)"
 * - Complete (after hangup): "Username: Call start: 2030-11-03 14:30:00 Call
 * End: 2030-11-03 14:45:00 "Duration: 15min""
 *
 * Multi-Call Tracking:
 * - Ticket.callId field stores comma-separated callIds: "abc123,def456,ghi789"
 * - Each call gets its own comment line with unique callId
 * - Multiple users can have active calls on same ticket simultaneously
 */
class CallController : public Controller {
public:
  /**
   * @brief Construct CallController with system dependencies
   *
   * @param ticketSystem Reference to ticket management system plugin
   * @param addressSystem Reference to address/contact lookup system plugin
   */
  CallController(TicketSystem &ticketSystem, AddressSystem &addressSystem);
  ~CallController() override;

  /**
   * @brief Main entry point for processing call events
   *
   * Parses JSON payload and sends "Accepted" HTTP response immediately
   * to avoid webhook timeouts.
   *
   * @param payload JSON webhook from phone system
   * @param urlParams URL parameters (unused for call events)
   * @return HTTP status code (0 on error, 1+ on success)
   */
  int run(std::istream &payload, std::string &urlParams) override;

  /**
   * @brief Process call event and route to appropriate handler
   *
   * Parses JSON to determine event type and delegates to:
   * - handleRing() for "Incoming Call" or "Outgoing Call"
   * - handleAcceptedCall() for "Accepted Call"
   * - handleTransferCall() for "Transfer Call"
   * - handleHangup() for "Hangup"
   *
   * @param payload JSON webhook body
   * @param urlParams URL parameters (unused)
   * @return 0 on success, error code on failure
   */
  int processCallEvent(std::istream &payload, std::string &urlParams);

private:
  // ========================================================================
  // Main Event Handlers
  // ========================================================================

  /**
   * @brief Handle ring/incoming/outgoing call event
   *
   * Workflow:
   * 1. Validate user exists (if provided)
   * 2. Lookup caller info in CardDAV (name, company, project IDs)
   * 3. Find existing ticket by project ID or create new one
   * 4. Set assignee if user provided (outgoing calls)
   * 5. Save ticket with callId
   *
   * @param call Call object with phoneNumber, callId, user, event
   * @return 0 on success, 1 on non-critical error (e.g., user doesn't exist)
   */
  int handleRing(Call &call);

  /**
   * @brief Handle accepted call event (call connected)
   *
   * Workflow:
   * 1. Find ticket by callId
   * 2. Set status to "In Progress"
   * 3. Record call start timestamp (first call only)
   * 4. Add comment with username, timestamp, and callId
   * 5. Save ticket
   *
   * Comment format: "Username: Call start: YYYY-MM-DD HH:MM:SS (callId)"
   *
   * @param call Call object with callId, user
   * @return 0 on success, 1 on error
   */
  int handleAcceptedCall(Call &call);

  /**
   * @brief Handle call transfer event (call reassigned to different user)
   *
   * Workflow:
   * 1. Find ticket by callId
   * 2. Update assignee to new user
   * 3. Find comment line with callId
   * 4. Update username in comment (keep timestamp/callId)
   * 5. Save ticket
   *
   * @param call Call object with callId, user (new assignee)
   * @return 0 on success, 1 on error
   */
  int handleTransferCall(Call &call);

  /**
   * @brief Handle call hangup event (call ended)
   *
   * Workflow:
   * 1. Find ticket by callId
   * 2. Record call end timestamp
   * 3. Find comment line with callId
   * 4. Extract start time from comment
   * 5. Calculate duration (DST-aware)
   * 6. Update comment with end time and duration
   * 7. Remove callId from tracking field
   * 8. Save ticket
   *
   * Final comment format:
   * "Username: Call start: START Call End: END "Duration: Xmin""
   *
   * @param call Call object with callId
   * @return 0 on success, throws exception on critical error
   */
  int handleHangup(Call &call);

  // ========================================================================
  // Helper Methods - Comment Parsing and Manipulation
  // ========================================================================

  /**
   * @brief Find position of callId in comma-separated list
   * @deprecated Legacy method - not currently used
   */
  int findCallIdPosition(const std::string &callIdList,
                         const std::string &targetCallId);

  /**
   * @brief Replace comment between delimiters
   * @deprecated Legacy method - not currently used
   */
  bool replaceDelimitedComment(std::string &description, int position,
                               const std::string &startDelimiter,
                               const std::string &endDelimiter,
                               const std::string &newComment);

  /**
   * @brief Find and replace comment line by callId
   * @deprecated Legacy method - superseded by updateCommentLineUsername
   */
  bool findAndReplaceCommentLineByCallId(std::string &description,
                                         const std::string &callId,
                                         const std::string &newCommentLine,
                                         std::string &extractedStartTime);

  /**
   * @brief Extract start time from comment by callId
   * @deprecated Legacy method - superseded by extractTimestampFromComment
   */
  bool extractStartTimeFromCommentByCallId(const std::string &description,
                                           const std::string &callId,
                                           std::string &extractedStartTime);

  // ========================================================================
  // Helper Methods - Hangup Event Processing
  // ========================================================================

  /**
   * @brief Find comment line containing specific callId
   *
   * Searches for pattern "(callId)" in description and returns line start
   * position.
   *
   * @param description Ticket description with multiple comment lines
   * @param callId Unique call identifier to search for
   * @return Line start position, or string::npos if not found
   */
  size_t findCommentLineByCallId(const std::string &description,
                                 const std::string &callId);

  /**
   * @brief Extract timestamp from comment line
   *
   * Parses comment format: "Username: Call start: YYYY-MM-DD HH:MM:SS (callId)"
   * to extract the timestamp portion.
   *
   * @param commentLine Single comment line from ticket description
   * @return Timestamp string "YYYY-MM-DD HH:MM:SS", or empty on parse failure
   */
  std::string extractTimestampFromComment(const std::string &commentLine);

  /**
   * @brief Calculate call duration in minutes with DST awareness
   *
   * Uses std::mktime with tm_isdst=-1 to handle daylight saving time
   * transitions.
   *
   * @param startTimestamp Start time "YYYY-MM-DD HH:MM:SS"
   * @param endTimestamp End time "YYYY-MM-DD HH:MM:SS"
   * @return Duration in minutes, or -1 on parse error
   */
  int calculateDurationMinutes(const std::string &startTimestamp,
                               const std::string &endTimestamp);

  /**
   * @brief Format completed comment line with duration
   *
   * Creates final comment format:
   * "Username: Call start: START Call End: END "Duration: Xmin""
   *
   * @param username User who handled the call
   * @param startTime Call start timestamp
   * @param endTime Call end timestamp
   * @param duration Duration string (e.g., "15")
   * @return Formatted comment line
   */
  std::string formatCompletedComment(const std::string &username,
                                     const std::string &startTime,
                                     const std::string &endTime,
                                     const std::string &duration);

  /**
   * @brief Remove callId from comma-separated list
   *
   * Wrapper for ticketSystem.removeCallIdFromExisting()
   *
   * @param callIdList Comma-separated callId list (modified in place)
   * @param callIdToRemove CallId to remove from list
   * @return Updated callId list
   */
  std::string removeCallIdFromList(std::string &callIdList,
                                   const std::string &callIdToRemove);

  // ========================================================================
  // Helper Methods - Accepted Call Event Processing
  // ========================================================================

  /**
   * @brief Get current timestamp in format "YYYY-MM-DD HH:MM:SS"
   *
   * Uses local time for timezone-aware timestamps.
   *
   * @return Current timestamp string
   */
  std::string getCurrentTimestamp();

  /**
   * @brief Check if call is already recorded in description
   *
   * Prevents duplicate comments by searching for existing comment with
   * matching user and callId.
   *
   * @param description Ticket description to search
   * @param user Username to search for
   * @param callId CallId to search for
   * @return true if comment exists, false if new comment needed
   */
  bool isCallAlreadyRecorded(const std::string &description,
                             const std::string &user,
                             const std::string &callId);

  /**
   * @brief Format call start comment line
   *
   * Creates comment format: "Username: Call start: TIMESTAMP (callId)"
   *
   * @param user Username handling the call
   * @param timestamp Call start timestamp
   * @param callId Unique call identifier
   * @return Formatted comment line
   */
  std::string formatCallStartComment(const std::string &user,
                                     const std::string &timestamp,
                                     const std::string &callId);

  // ========================================================================
  // Helper Methods - Transfer Call Event Processing
  // ========================================================================

  /**
   * @brief Update username in comment line for transferred call
   *
   * Finds comment line with callId and replaces username prefix while
   * preserving timestamp and callId.
   *
   * @param description Ticket description (modified in place)
   * @param callId CallId to identify which comment to update
   * @param newUsername New assignee username
   * @return true on success, false if comment not found or parse error
   */
  bool updateCommentLineUsername(std::string &description,
                                 const std::string &callId,
                                 const std::string &newUsername);

  /**
   * @brief Update ticket status to "In Progress" for transfer
   *
   * Only updates if ticket is not already closed.
   *
   * @param ticket Ticket to update (non-null)
   * @return true if status changed, false if already closed
   */
  bool updateTicketStatusForTransfer(Ticket *ticket);

  // ========================================================================
  // Helper Methods - Ring Event Processing
  // ========================================================================

  /**
   * @brief Validate that user exists in ticket system
   *
   * Checks if call.user is in ticket system user list. Used to filter out
   * calls from unknown users.
   *
   * @param call Call object with user field
   * @return true if user exists or user field empty, false if user unknown
   */
  bool validateUserExists(const Call &call);

  /**
   * @brief Find or create ticket for known contact (has project IDs)
   *
   * Searches for existing ticket in any of the contact's projects.
   * If found, appends callId. If not found, creates new ticket in first
   * project.
   *
   * @param addressInfo Contact info from CardDAV (must have project IDs)
   * @param call Call object with callId
   * @return Ticket pointer (caller owns memory)
   */
  Ticket *findOrCreateTicketForKnownContact(
      const AddressSystem::addressInformation &addressInfo, const Call &call);

  /**
   * @brief Find or create ticket for unknown number (no project IDs)
   *
   * Searches by contact name or phone number in default location.
   * Creates new ticket in default project if not found.
   *
   * @param addressInfo Contact info (may be partial - no project IDs)
   * @param call Call object with phoneNumber and callId
   * @return Ticket pointer (caller owns memory)
   */
  Ticket *findOrCreateTicketForUnknownNumber(
      const AddressSystem::addressInformation &addressInfo, const Call &call);

  // ========================================================================
  // Helper Methods - Ticket Search
  // ========================================================================

  /**
   * @brief Find existing open ticket in any of the provided projects
   *
   * Searches each project for a New or In Progress Call ticket.
   * Returns first match found.
   *
   * @param ids Vector of ticket system project IDs to search
   * @return Ticket pointer if found (caller owns), nullptr if not found
   */
  Ticket *
  getExistingTicketByProjectIds(const std::vector<std::string> ids) const;

  /**
   * @brief Find existing open ticket by name in default project
   *
   * Searches default project (configUnknownNumberSaveLocation) for ticket
   * with matching name.
   *
   * @param name Contact name or phone number to search
   * @return Ticket pointer if found (caller owns), nullptr if not found
   */
  Ticket *getExistingTicketByName(const std::string &name) const;

  // ========================================================================
  // System Dependencies
  // ========================================================================
  TicketSystem &ticketSystem;   ///< Ticket management system plugin
  AddressSystem &addressSystem; ///< Address/contact lookup system plugin
};
