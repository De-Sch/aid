/**
 * @file UiController.cpp
 * @brief Implementation of UI request handling logic
 *
 * This file implements the UiController class which processes HTTP requests
 * from the SvelteKit frontend. It coordinates between three major systems:
 * - TicketSystem plugin for work package management
 * - AddressSystem plugin for contact/company lookups
 * - Ui component for response formatting and SSE communication
 */

#include "Controllers/UiController.h"
#include "Logger/Logger.h"
#include "Models/Ticket.h"
#include "Systems/AddressSystem.h"
#include "Systems/TicketSystem.h"
#include "Ui/Ui.h"
#include "json.hpp"

#include <exception>
#include <fstream>
#include <memory>

/**
 * @brief Constructor initializes controller with system dependencies
 */
UiController::UiController(TicketSystem &ticketSystem,
                           AddressSystem &addressSystem, Ui &ui)
    : ticketSystem(ticketSystem), addressSystem(addressSystem), ui(ui) {}

/**
 * @brief Destructor cleans up resources
 */
UiController::~UiController() {}

/**
 * @brief Main entry point for UI request processing
 *
 * Delegates to routeUiRequest() for actual handling. This method serves
 * as the Controller interface implementation.
 */
int UiController::run(std::istream &payload, std::string &urlParams) {
  logging::Logger::debug("run Controller");
  int result = routeUiRequest(payload, urlParams);
  return result;
}

/**
 * @brief Route incoming request to appropriate handler based on URL pattern
 *
 * URL Routing:
 * - "/ui/comment/..." -> handleCommentSubmission()
 * - "/ui/close/..."   -> handleTicketClosure()
 * - "/ui/..."         -> handleDashboardRequest()
 *
 * @return 1 on successful routing and handling, 0 on error
 */
int UiController::routeUiRequest(std::istream &payload,
                                 std::string &urlParams) {
  logging::Logger::debug("start actionUI");
  int result = 0;

  // Route based on URL pattern (order matters - check most specific first)
  if (urlParams.find("comment") != std::string::npos)
    result = handleCommentSubmission(payload, urlParams);
  else if (urlParams.find("close") != std::string::npos)
    result = handleTicketClosure(payload, urlParams);
  else if (urlParams.find("ui") != std::string::npos)
    result = handleDashboardRequest(payload, urlParams);

  if (result == 0) {
    logging::Logger::error(
        "UiController action failed: Method not found or handling failed");
    // Note: Individual methods now send their own error responses via
    // ui.sendActionResult
  }
  return result;
}

// ============================================================================
// Helper Methods - Dashboard Request
// ============================================================================

/**
 * @brief Extract username from URL path
 *
 * Parses URL format: "/ui/dashboard/username" -> extracts "username"
 * Used for user-specific dashboard filtering.
 *
 * @return Username string after second slash, or empty if not found
 */
std::string UiController::extractUserFromUrl(const std::string &urlParams) {
  size_t pos = urlParams.find("/", 1);
  if (pos != std::string::npos) {
    return urlParams.substr(pos + 1);
  }
  return "";
}

/**
 * @brief Read payload content if stream has available data
 *
 * Checks if payload stream is valid and has content before reading.
 * Used for dashboard requests that may or may not include search criteria.
 *
 * @return Full payload as string, or empty if no data available
 */
std::string UiController::readPayloadIfAvailable(std::istream &payload) {
  if (payload.good() && payload.peek() != EOF) {
    return std::string(std::istreambuf_iterator<char>(payload), {});
  }
  return "";
}

/**
 * @brief Fetch and combine data from both AddressSystem and TicketSystem
 *
 * Workflow:
 * 1. If payload provided -> call AddressSystem::getDashboardInformation()
 * 2. Always call TicketSystem::getDashboardInformation()
 * 3. If both responses exist -> combine using
 * Ui::combineCallInfoAndTicketsForDashboard()
 * 4. Otherwise return ticket data only
 *
 * @param stringPayload JSON search criteria for address lookup
 * @param urlParams URL parameters (unused in current implementation)
 * @return Combined JSON string, or empty on critical error
 */
std::string
UiController::fetchAndCombineDashboardData(const std::string &stringPayload,
                                           std::string &urlParams) {
  std::string addressSystemResponse = "";
  std::string ticketSystemResponse = "";

  // Get address information if payload is provided (optional contact/company
  // search)
  if (!stringPayload.empty()) {
    std::istringstream payloadStream(stringPayload);
    addressSystemResponse =
        addressSystem.getDashboardInformation(payloadStream, urlParams);
    logging::Logger::debug("AddressSystem response received");
  }

  // Always get ticket information (required for dashboard)
  std::istringstream emptyStream("");
  ticketSystemResponse =
      ticketSystem.getDashboardInformation(emptyStream, urlParams);

  if (ticketSystemResponse.empty()) {
    logging::Logger::error("TicketSystem->getDashboardInformation() failed");
    return "";
  }

  // Combine responses if both are available, otherwise return tickets only
  if (!addressSystemResponse.empty() && !ticketSystemResponse.empty()) {
    return ui.combineCallInfoAndTicketsForDashboard(addressSystemResponse,
                                                    ticketSystemResponse);
  } else {
    return ticketSystemResponse;
  }
}

// ============================================================================
// Main Request Handlers
// ============================================================================

/**
 * @brief Handle dashboard data request from SvelteKit frontend
 *
 * Request Flow:
 * 1. Extract username from URL (optional)
 * 2. Read payload if present (address search criteria)
 * 3. Fetch data from AddressSystem and TicketSystem
 * 4. Combine responses if both available
 * 5. Send result to UI via SSE connection
 *
 * @param payload Optional JSON body with address search parameters
 * @param urlParams URL path (e.g., "/ui/dashboard" or "/ui/dashboard/username")
 * @return 1 on success, 0 on error
 */
int UiController::handleDashboardRequest(std::istream &payload,
                                         std::string &urlParams) {
  logging::Logger::debug("handleDashboardRequest started");

  // Extract user from URL (optional, for user-specific dashboards)
  std::string user = extractUserFromUrl(urlParams);

  // Read payload if available (contains address search criteria)
  std::string stringPayload = readPayloadIfAvailable(payload);
  logging::Logger::debug("Payload content: '" + stringPayload + "'");

  // Fetch and combine dashboard data from both systems
  std::string dashboardData =
      fetchAndCombineDashboardData(stringPayload, urlParams);

  if (dashboardData.empty()) {
    return 0; // Error already logged in fetchAndCombineDashboardData
  }

  // Send combined data to UI via SSE
  std::istringstream dashboardStream(dashboardData);
  ui.apiToUi(dashboardStream);

  return 1;
}

// ============================================================================
// Helper Methods - Comment Submission
// ============================================================================

/**
 * @brief Extract ticket ID from URL path
 *
 * Parses URL format: "/ui/comment/251" or "/ui/close/251" -> extracts "251"
 * The ticket ID is always the last path segment.
 *
 * @param urlParams Full URL path
 * @return Ticket ID string, or empty if URL format is invalid
 */
std::string UiController::extractTicketIdFromUrl(const std::string &urlParams) {
  size_t lastSlash = urlParams.find_last_of("/");
  if (lastSlash == std::string::npos) {
    logging::Logger::error("Invalid URL format, no ticket ID found in: " +
                           urlParams);
    return "";
  }
  return urlParams.substr(lastSlash + 1);
}

/**
 * @brief Parse comment text from JSON payload
 *
 * Expected JSON format: {"comment": "text"}
 * Validates that payload is not empty and contains the "comment" field.
 *
 * @param payload Input stream containing JSON body
 * @return Comment text, or empty string on parse/validation error
 */
std::string UiController::parseCommentFromPayload(std::istream &payload) {
  // Read entire payload into string
  std::string payloadString(std::istreambuf_iterator<char>(payload), {});
  if (payloadString.empty()) {
    logging::Logger::error("Empty payload received");
    return "";
  }

  // Parse JSON and validate structure
  nlohmann::json commentData = nlohmann::json::parse(payloadString);
  if (!commentData.contains("comment")) {
    logging::Logger::error("No 'comment' field in payload");
    return "";
  }

  // Extract and validate comment text
  std::string comment = commentData["comment"];
  if (comment.empty()) {
    logging::Logger::error("Empty comment text");
    return "";
  }

  return comment;
}

/**
 * @brief Append new comment to ticket's description field
 *
 * Comments are appended with newline separation. If the description is empty,
 * the comment becomes the first line.
 *
 * @param ticket Ticket to modify (must be non-null)
 * @param comment Comment text to append
 */
void UiController::appendCommentToTicket(Ticket *ticket,
                                         const std::string &comment) {
  if (!ticket->description.empty()) {
    ticket->description += "\n" + comment;
  } else {
    ticket->description = comment;
  }
}

/**
 * @brief Handle comment submission from UI
 *
 * Request Flow:
 * 1. Extract ticket ID from URL path ("/ui/comment/251")
 * 2. Parse comment text from JSON payload ({"comment": "text"})
 * 3. Retrieve ticket from ticket system
 * 4. Append comment to ticket description
 * 5. Save ticket back to ticket system
 * 6. Send success/failure result to UI via SSE
 *
 * @param payload JSON body containing comment text
 * @param urlParams URL path with ticket ID
 * @return 1 on success, 0 on error
 */
int UiController::handleCommentSubmission(std::istream &payload,
                                          std::string &urlParams) {
  logging::Logger::debug("handleCommentSubmission started for URL: " +
                         urlParams);

  // Extract and validate ticket ID from URL
  std::string ticketId = extractTicketIdFromUrl(urlParams);
  if (ticketId.empty()) {
    return 0; // Error already logged
  }

  // Parse and validate comment from JSON payload
  std::string comment = parseCommentFromPayload(payload);
  if (comment.empty()) {
    return 0; // Error already logged
  }

  // Retrieve the ticket from the system
  std::unique_ptr<Ticket> ticket(ticketSystem.getTicketById(ticketId));
  if (!ticket) {
    logging::Logger::error("Ticket not found: " + ticketId);
    ui.sendActionResult(false, "COMMENT_SAVE", "Ticket not found", ticketId);
    return 0;
  }

  // Append the new comment and save
  appendCommentToTicket(ticket.get(), comment);

  if (ticketSystem.saveTicket(ticket.get())) {
    logging::Logger::info("Comment saved successfully for ticket " + ticketId);
    ui.sendActionResult(true, "COMMENT_SAVE", "Comment saved successfully",
                        ticketId);
    return 1;
  } else {
    logging::Logger::error("Failed to save comment for ticket " + ticketId);
    ui.sendActionResult(false, "COMMENT_SAVE", "Failed to save comment",
                        ticketId);
    return 0;
  }
}

/**
 * @brief Handle ticket closure request from UI
 *
 * Request Flow:
 * 1. Extract ticket ID from URL path ("/ui/close/251")
 * 2. Retrieve ticket from ticket system to verify it exists
 * 3. Close ticket using TicketSystem::closeTicket() (sets status to "closed")
 * 4. Send success/failure result to UI via SSE
 *
 * Note: Payload is unused - status is always set to "closed"
 *
 * @param payload Request body (unused)
 * @param urlParams URL path with ticket ID
 * @return 1 on success, 0 on error
 */
int UiController::handleTicketClosure(std::istream &payload,
                                      std::string &urlParams) {
  logging::Logger::debug("handlingCloseTicket START");
  logging::Logger::debug("URL: " + urlParams);
  int result = 0;

  // Extract ticket ID from URL params like "/ui/close/251"
  size_t lastSlash = urlParams.find_last_of("/");
  if (lastSlash == std::string::npos) {
    logging::Logger::error(
        "handlingCloseTicket: Invalid URL format, no ticket ID found");
    return 0;
  }

  std::string ticketId = urlParams.substr(lastSlash + 1);
  logging::Logger::debug("handlingCloseTicket TICKET ID: " + ticketId);

  // Status is always "closed" for UI-based closure (no payload parsing needed)
  std::string status = "closed";
  logging::Logger::debug("handlingCloseTicket STATUS: " + status);

  // Get the ticket first to verify it exists
  logging::Logger::debug("handlingCloseTicket BEFORE GET TICKET BY ID");
  std::unique_ptr<Ticket> ticket(ticketSystem.getTicketById(ticketId));
  logging::Logger::debug("handlingCloseTicket AFTER GET TICKET BY ID");

  if (!ticket) {
    logging::Logger::error("No ticket found with ID: " + ticketId);
    ui.sendActionResult(false, "TICKET_CLOSE", "Ticket not found", ticketId);
    return 0;
  }

  logging::Logger::debug("handlingCloseTicket TICKET FOUND: " + ticket->title);
  logging::Logger::info("handlingCloseTicket: Found ticket to close: " +
                        ticket->title + " (ID: " + ticketId + ")");

  // Close the ticket using the ticket system
  if (ticketSystem.closeTicket(ticket.get(), status)) {
    logging::Logger::info("Ticket closed successfully: " + ticketId);
    ui.sendActionResult(true, "TICKET_CLOSE", "Ticket closed successfully",
                        ticketId);
    result = 1;
  } else {
    logging::Logger::error("Failed to close ticket: " + ticketId);
    ui.sendActionResult(false, "TICKET_CLOSE", "Failed to close ticket",
                        ticketId);
  }

  return result;
}
