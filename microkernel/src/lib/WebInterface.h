/**
 * @file WebInterface.h
 * @brief CGI-based web interface implementation for AID system
 *
 * Architecture Overview:
 * - Implements Ui interface for microkernel communication
 * - Handles CGI request/response conversion for SvelteKit frontend
 * - Manages CORS headers for cross-origin API calls
 * - Provides JSON-based data transformation between backend and UI
 *
 * Integration Points:
 * - Receives backend data (call info, tickets) and formats for web UI
 * - Handles HTTP methods (GET, POST, OPTIONS) with proper CORS support
 * - Bridges microkernel plugin architecture with web-based dashboard
 */
#pragma once
#include "Models/Call.h"
#include "Models/Ticket.h"
#include "Ui/Ui.h"

/**
 * @class WebInterface
 * @brief CGI-based web interface for AID system dashboard
 *
 * Responsibilities:
 * - Convert backend API responses to UI-friendly JSON format
 * - Handle CGI request/response cycle with proper HTTP headers
 * - Manage CORS preflight (OPTIONS) and actual requests
 * - Combine call information with ticket data for dashboard view
 *
 * Usage:
 * - Loaded as shared library by microkernel plugin system
 * - Instantiated via createUi() factory function from config.json
 * - Communicates via stdin/stdout following CGI specification
 */
class WebInterface : public Ui {
public:
  /** @brief Default constructor - initializes empty WebInterface */
  WebInterface();

  /**
   * @brief Configuration constructor - loads web base URL from config
   * @param config JSON configuration containing projectWebBaseUrl
   */
  WebInterface(nlohmann::json &config);

  /** @brief Destructor - cleans up WebInterface resources */
  ~WebInterface();

  /** @brief JSON data cache for UI state (currently unused) */
  nlohmann::json uiData;

  /**
   * @brief Converts backend API response to UI JSON format
   *
   * Workflow:
   * 1. Parse JSON from backend response stream
   * 2. Check HTTP request method and set CORS headers
   * 3. Output JSON to stdout for CGI response
   * 4. Log response for debugging
   *
   * @param response Backend API response stream (JSON format)
   * @return JSON string formatted for web UI, or empty string on failure
   * @throws nlohmann::json::exception on JSON parsing errors
   */
  std::string apiToUi(std::istream &response) override;

  /**
   * @brief Converts UI request to backend API format (partially implemented)
   *
   * @param request UI request stream from CGI stdin
   * @return Formatted request string for backend API
   * @note Currently has incomplete implementation - mostly placeholder
   */
  std::string uiToApi(std::istream &request) override;

  /**
   * @brief Combines call information and tickets into unified dashboard JSON
   *
   * Creates dashboard data structure:
   * {
   *   "call": { callId, caller, projectIds, ... },
   *   "tickets": [ { id, title, status, ... }, ... ]
   * }
   *
   * @param call JSON string with active call information
   * @param tickets JSON string array with user's ticket list
   * @return Combined JSON string with both call and ticket data
   */
  std::string
  combineCallInfoAndTicketsForDashboard(std::string &call,
                                        std::string &tickets) override;

  /**
   * @brief Sends action result (success/error) back to UI
   *
   * Response format:
   * {
   *   "status": "SUCCESS" | "ERROR",
   *   "operation": "comment" | "close_ticket" | ...,
   *   "message": "Result",
   *   "ticketId": "123" (optional),
   *   "timestamp": unix_timestamp
   * }
   *
   * @param success True if operation succeeded, false on error
   * @param operation Operation name (e.g., "comment", "close_ticket")
   * @param message Result message
   * @param ticketId Optional ticket ID for ticket-related operations
   */
  void sendActionResult(bool success, const std::string &operation,
                        const std::string &message,
                        const std::string &ticketId = "") override;

private:
  /**
   * @brief Safely extracts configuration value with error handling
   *
   * @tparam T Value type (string, int, etc.)
   * @param config JSON configuration object
   * @param param Configuration parameter name
   * @param defaultVal Default value if parameter missing
   * @param hasError Set to true if parameter was missing
   * @return Extracted value or default if not found
   */
  template <typename T>
  T getConfigValue(nlohmann::json &config, const char *param,
                   const T &defaultVal, bool &hasError);

  /**
   * @brief Checks HTTP method and sets appropriate CORS headers
   *
   * Handles:
   * - OPTIONS: Preflight CORS check (returns false to skip backend processing)
   * - GET/POST: Sets CORS headers and continues to backend (returns true)
   *
   * Headers set:
   * - Content-Type: application/json
   * - Access-Control-Allow-Origin: *
   * - Access-Control-Allow-Methods: POST, GET, UPDATE, DELETE, OPTIONS
   * - Access-Control-Allow-Headers: Content-Type
   *
   * @param requestMethod HTTP request method from REQUEST_METHOD env var
   * @return true if backend processing needed, false if OPTIONS handled
   */
  bool checkMethodAndSetHeader(std::string requestMethod);

  /**
   * @brief Extracts CGI endpoint path from PATH_INFO environment variable
   *
   * @return Path string with leading slash removed (e.g., "dashboard/user")
   */
  std::string endpointPath();
};
