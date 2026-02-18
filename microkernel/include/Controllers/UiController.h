/**
 * @file UiController.h
 * @brief Controller for handling SvelteKit UI requests
 *
 * This controller processes HTTP requests from the SvelteKit frontend,
 * including dashboard data retrieval, comment submissions, and ticket closure
 * actions. It coordinates between the UI layer, TicketSystem plugin, and
 * AddressSystem plugin to provide unified data to the frontend.
 */

#pragma once
#include "Controller.h"
#include "json.hpp"
#include <string>

struct Ticket;
class Ui;
class TicketSystem;
class AddressSystem;

/**
 * @class UiController
 * @brief Handles web UI requests and coordinates data between systems
 *
 * Request Flow:
 * 1. run() receives HTTP request from SvelteKit frontend
 * 2. routeUiRequest() dispatches to appropriate handler based on URL
 * 3. Handler processes request (dashboard, comment, close)
 * 4. Results sent back through Ui::sendActionResult() or Ui::apiToUi()
 *
 * Supported Actions:
 * - GET  /ui/dashboard/{username} - Fetch combined ticket and address data
 * - POST /ui/comment/{ticketId}   - Add comment to ticket
 * - POST /ui/close/{ticketId}     - Close ticket
 */
class UiController : public Controller {
public:
  /**
   * @brief Construct UiController with system dependencies
   *
   * @param ticketSystem Reference to ticket management system plugin
   * @param addressSystem Reference to address/contact lookup system plugin
   * @param ui Reference to UI component for sending responses
   */
  UiController(TicketSystem &ticketSystem, AddressSystem &addressSystem,
               Ui &ui);
  ~UiController() override;

  /**
   * @brief Main entry point for processing UI requests
   *
   * @param payload Request body (JSON for POST, empty for GET)
   * @param urlParams URL path (e.g., "/ui/dashboard/username")
   * @return 1 on success, 0 on error
   */
  int run(std::istream &payload, std::string &urlParams) override;

  /**
   * @brief Route UI requests to appropriate handlers based on URL
   *
   * @param payload Request body stream
   * @param urlParams URL path to determine routing
   * @return 1 on success, 0 on error
   */
  int routeUiRequest(std::istream &payload, std::string &urlParams);

private:
  // === Main Request Handlers ===

  /**
   * @brief Handle dashboard data request (GET /ui/dashboard/{username})
   *
   * Fetches and combines:
   * - Ticket information from TicketSystem (always)
   * - Address information from AddressSystem (if payload provided)
   *
   * @param payload Optional JSON body with search criteria
   * @param urlParams URL containing optional username
   * @return 1 on success, 0 on error
   */
  int handleDashboardRequest(std::istream &payload, std::string &urlParams);

  /**
   * @brief Handle comment submission (POST /ui/comment/{ticketId})
   *
   * Appends new comment to ticket description in ticket system.
   *
   * @param payload JSON body: {"comment": "text"}
   * @param urlParams URL containing ticket ID (e.g., "/ui/comment/251")
   * @return 1 on success, 0 on error
   */
  int handleCommentSubmission(std::istream &payload, std::string &urlParams);

  /**
   * @brief Handle ticket closure request (POST /ui/close/{ticketId})
   *
   * Closes ticket in ticket system by updating its status.
   *
   * @param payload Request body (unused, status is always "closed")
   * @param urlParams URL containing ticket ID (e.g., "/ui/close/251")
   * @return 1 on success, 0 on error
   */
  int handleTicketClosure(std::istream &payload, std::string &urlParams);

  // === Helper Methods - Comment Submission ===

  /**
   * @brief Extract ticket ID from URL path
   *
   * @param urlParams URL like "/ui/comment/251" or "/ui/close/251"
   * @return Ticket ID string (e.g., "251"), or empty on error
   */
  std::string extractTicketIdFromUrl(const std::string &urlParams);

  /**
   * @brief Parse comment text from JSON payload
   *
   * @param payload Stream containing JSON: {"comment": "text"}
   * @return Comment text, or empty string on parse error
   */
  std::string parseCommentFromPayload(std::istream &payload);

  /**
   * @brief Append new comment to ticket's description field
   *
   * @param ticket Ticket to update (non-null)
   * @param comment Comment text to append (separated by newline)
   */
  void appendCommentToTicket(Ticket *ticket, const std::string &comment);

  // === Helper Methods - Dashboard Request ===

  /**
   * @brief Extract username from URL path
   *
   * @param urlParams URL like "/ui/dashboard/username"
   * @return Username string, or empty if not present
   */
  std::string extractUserFromUrl(const std::string &urlParams);

  /**
   * @brief Read payload content if stream has data
   *
   * @param payload Input stream to read
   * @return Payload string, or empty if no data available
   */
  std::string readPayloadIfAvailable(std::istream &payload);

  /**
   * @brief Fetch data from both systems and combine into single response
   *
   * Calls:
   * 1. AddressSystem::getDashboardInformation() if payload provided
   * 2. TicketSystem::getDashboardInformation() always
   * 3. Ui::combineCallInfoAndTicketsForDashboard() if both present
   *
   * @param stringPayload JSON search criteria for address lookup
   * @param urlParams URL parameters (unused currently)
   * @return Combined JSON response, or empty on error
   */
  std::string fetchAndCombineDashboardData(const std::string &stringPayload,
                                           std::string &urlParams);

  // === System Dependencies ===
  TicketSystem &ticketSystem;   ///< Ticket management system plugin
  AddressSystem &addressSystem; ///< Address/contact lookup system plugin
  Ui &ui;                       ///< UI component for response formatting
};
