/**
 * @file WebInterface.cpp
 * @brief CGI-based web interface implementation for AID system
 *
 * Detailed Description:
 * This file implements the WebInterface plugin that bridges the microkernel
 * backend with the SvelteKit web frontend. It handles HTTP/CGI communication
 * including:
 * - JSON request/response transformation
 * - CORS header management for cross-origin requests
 * - HTTP method handling (GET, POST, OPTIONS)
 * - Dashboard data aggregation (call info + tickets)
 *
 * CGI Environment:
 * - Reads REQUEST_METHOD environment variable for HTTP method detection
 * - Reads PATH_INFO environment variable for endpoint routing
 * - Outputs to stdout following CGI specification
 * - Sets HTTP headers before body content
 */
#include "WebInterface.h"
#include "Logger/Logger.h"
#include "Models//Call.h"
#include "Models/Ticket.h"
#include <fstream>
#include <iostream>

//==============================================================================
// Constructors and Destructor
//==============================================================================

/** @brief Default constructor - initializes empty WebInterface instance */
WebInterface::WebInterface() : Ui() {}

/** @brief Destructor - no special cleanup needed */
WebInterface::~WebInterface() {}

/**
 * @brief Configuration constructor - loads web base URL from JSON config
 *
 * Configuration parameters loaded:
 * - libPath: Path to shared library (for logging)
 * - projectWebBaseUrl: Base URL for ticket system web interface
 *
 * Error handling:
 * - Missing config values trigger error logging
 * - Default empty strings used for missing parameters
 *
 * @param config JSON configuration object from config.json
 */
WebInterface::WebInterface(nlohmann::json &config) : Ui(config) {
  bool err = false;

  logging::Logger::info(
      "Try to load WebInterface from: " +
      getConfigValue(config, "libPath", std::string(""), err) + "...");
  logging::Logger::info(
      getConfigValue(config, "projectWebBaseUrl", std::string(""), err));
  url = getConfigValue(config, "projectWebBaseUrl", std::string(""), err);

  if (!err) {
    logging::Logger::info("WebInterface loaded without issues.");
  } else {
    logging::Logger::error(
        "Missing Config values for WebInterface, template has been written.");
  }
}

//==============================================================================
// Core API Transformation Methods
//==============================================================================

/**
 * @brief Converts backend API response to UI JSON format with proper CGI
 * headers
 *
 * Workflow:
 * 1. Parse incoming JSON response from backend API
 * 2. Get HTTP request method from REQUEST_METHOD environment variable
 * 3. Set appropriate HTTP/CORS headers via checkMethodAndSetHeader()
 * 4. Output formatted JSON to stdout (CGI body)
 * 5. Flush output to ensure immediate delivery
 * 6. Log response for debugging
 *
 * CORS handling:
 * - OPTIONS requests return early (preflight check)
 * - Actual requests continue with full JSON response
 *
 * @param response Backend API response stream containing JSON
 * @return JSON string (also written to stdout), or empty string if OPTIONS
 * request
 * @throws nlohmann::json::exception on JSON parse errors
 * @throws Generic exception for internal server errors
 */
std::string WebInterface::apiToUi(std::istream &response) {
  try {
    // Parse JSON from backend API response
    nlohmann::json result = nlohmann::json::parse(response);

    // Get HTTP request method from CGI environment
    const char *methodEnv = getenv("REQUEST_METHOD");
    std::string requestMethod = methodEnv ? methodEnv : "";

    // Set HTTP headers and check if backend processing is needed
    bool check = checkMethodAndSetHeader(requestMethod);
    if (!check)
      return ""; // OPTIONS request handled, no backend data needed

    // Output JSON response to stdout (CGI body)
    std::cout << result.dump(2) << std::endl;
    std::cout.flush(); // Force immediate output
    logging::Logger::info(result.dump(2));

    return result.dump();

  } catch (nlohmann::json::exception &e) {
    logging::Logger::error(std::string("apiToUi() failed \n") + e.what());
    throw;
  } catch (...) {
    logging::Logger::error("Status: 500 Internal Server Error\n");
    logging::Logger::error("Content-Type: text/plain\n\n");
    logging::Logger::error("Internal Server Error\n");
    logging::Logger::error("apiToUi() failed");
    throw;
  }
}

/**
 * @brief Converts UI request to backend API format (INCOMPLETE IMPLEMENTATION)
 *
 * Current state:
 * - Partially implemented with placeholder logic
 * - Attempts to parse incoming request JSON
 * - Checks for "name" parameter in path
 *
 * TODO:
 * - Complete request parsing and validation
 * - Implement proper routing based on endpoint path
 * - Add comprehensive error handling
 *
 * @param request UI request stream from CGI stdin
 * @return Formatted request JSON string, or empty string on error
 * @note This method needs significant work to be production-ready
 */
std::string WebInterface::uiToApi(std::istream &request) {
  try {
    // Empty try block - placeholder for future implementation
  } catch (nlohmann::json::parse_error &e) {
  }

  std::string path;
  request >> path;

  if (checkMethodAndSetHeader(path))
    if (path.find("name")) {
      try {
        nlohmann::json name = nlohmann::json::parse(request);
        nlohmann::json query = {};
        return name.dump(2);
      } catch (const nlohmann::json::exception &e) {
        logging::Logger::error(std::string("WebInterface::uiToApi(): ") +
                               e.what());
        return "";
      } catch (...) {
        logging::Logger::error("WebInterface::uiToApi() failed");
        return "";
      }
    }
  logging::Logger::error("WebInterface::uiToApi() failed");
  return "";
}

/**
 * @brief Combines call information and tickets into unified dashboard JSON
 *
 * Creates a single JSON object containing:
 * - call: Active call information (callId, caller, projectIds, etc.)
 * - tickets: Array of user's tickets (id, title, status, etc.)
 *
 * This combined format allows the dashboard to:
 * 1. Display active call status in header/banner
 * 2. Show list of tickets in main view
 * 3. Highlight the ticket associated with active call
 *
 * JSON structure:
 * {
 *   "call": { callId: "...", caller: "+49...", projectIds: [...], ... },
 *   "tickets": [ { id: 123, title: "...", status: "New", ... }, ... ]
 * }
 *
 * @param call JSON string with active call information (may be null)
 * @param tickets JSON string array with user's ticket list
 * @return Combined JSON string formatted for dashboard consumption
 * @throws nlohmann::json::exception if input strings are not valid JSON
 */
std::string
WebInterface::combineCallInfoAndTicketsForDashboard(std::string &call,
                                                    std::string &tickets) {
  // Parse both input JSON strings and combine into single object
  nlohmann::json resultJson = {{"callInformation", nlohmann::json::parse(call)},
                               {"tickets", nlohmann::json::parse(tickets)}};

  std::string result = resultJson.dump(2);

  return result;
}

//==============================================================================
// Private Helper Methods
//==============================================================================

/**
 * @brief Safely extracts configuration value with error handling
 *
 * Error recovery strategy:
 * - If parameter exists: return its value
 * - If parameter missing: set it to default in config, set error flag, return
 * default
 *
 * This allows config template generation for missing parameters while still
 * returning usable default values for continued operation.
 *
 * @tparam T Value type (string, int, etc.)
 * @param config JSON configuration object to read from
 * @param param Configuration parameter name to extract
 * @param defaultVal Default value to use if parameter missing
 * @param hasError Reference flag set to true if parameter was missing
 * @return Extracted value if found, defaultVal otherwise
 */
template <typename T>
T WebInterface::getConfigValue(nlohmann::json &config, const char *param,
                               const T &defaultVal, bool &hasError) {
  try {
    return config[param];
  } catch (nlohmann::json::exception &e) {
    config[param] = defaultVal;
    hasError = true;
    return defaultVal;
  }
}

/**
 * @brief Checks HTTP method and sets appropriate CORS headers for CGI response
 *
 * Two-phase CORS handling:
 *
 * Phase 1 - OPTIONS (Preflight):
 * - Browser sends OPTIONS before actual cross-origin request
 * - We return CORS headers with empty body
 * - Returns false to signal "no backend processing needed"
 *
 * Phase 2 - Actual Request (GET/POST/etc):
 * - Sets same CORS headers plus Content-Type
 * - Returns true to signal "continue with backend processing"
 *
 * Headers set:
 * - Content-Type: application/json (identifies response format)
 * - Access-Control-Allow-Origin: * (allow all origins)
 * - Access-Control-Allow-Methods: POST, GET, UPDATE, DELETE, OPTIONS
 * - Access-Control-Allow-Headers: Content-Type (allows JSON content in
 * requests)
 *
 * @param requestMethod HTTP method from REQUEST_METHOD environment variable
 * @return true if backend should process request, false if OPTIONS handled
 */
bool WebInterface::checkMethodAndSetHeader(std::string requestMethod) {
  // Handle OPTIONS preflight request (CORS Phase 1)
  if (requestMethod.find("OPTIONS") != std::string::npos) {
    std::cout << "Content-Type: application/json\n";
    std::cout << "Access-Control-Allow-Origin: *\n";
    std::cout << "Access-Control-Allow-Methods: POST, GET, OPTIONS\n";
    std::cout << "Access-Control-Allow-Headers: Content-Type\n";
    std::cout << "Content-Length: 0\n\n";
    logging::Logger::debug(
        "WebInterface: OPTIONS request handled, no backend action needed");
    return false; // No backend processing for OPTIONS
  }

  // Handle actual requests (CORS Phase 2)
  std::cout << "Content-Type: application/json\n";
  std::cout << "Access-Control-Allow-Origin: *\n";
  std::cout
      << "Access-Control-Allow-Methods: POST, GET, UPDATE, DELETE, OPTIONS\n";
  std::cout << "Access-Control-Allow-Headers: Content-Type\n\n";
  logging::Logger::debug(
      "WebInterface: Request headers set, backend action required");
  return true; // Continue to backend processing
}

/**
 * @brief Extracts CGI endpoint path from PATH_INFO environment variable
 *
 * CGI PATH_INFO format:
 * - Contains the path after the script name in the URL
 * - Example: /cgi-bin/aid.cgi/dashboard/user -> PATH_INFO="/dashboard/user"
 *
 * Processing:
 * - Reads PATH_INFO environment variable
 * - Removes leading slash for easier routing
 * - Returns empty string if PATH_INFO not set
 *
 * @return Endpoint path without leading slash (e.g., "dashboard/user")
 */
std::string WebInterface::endpointPath() {
  // Get the PATH_INFO environment variable from CGI
  const char *pathInfo = getenv("PATH_INFO");
  if (pathInfo == nullptr) {
    return "";
  }

  // Remove leading slash if present for easier routing logic
  std::string path(pathInfo);
  if (path.size() > 0 && path[0] == '/') {
    path = path.substr(1);
  }

  return path;
}

/**
 * @brief Sends action result (success/error) back to UI via CGI
 *
 * Use cases:
 * - Comment saved to ticket: success=true, operation="comment"
 * - Ticket closed: success=true, operation="close_ticket"
 * - Operation failed: success=false, operation="...", message=error details
 *
 * Response JSON structure:
 * {
 *   "status": "SUCCESS" | "ERROR",
 *   "operation": "comment" | "close_ticket" | ...,
 *   "message": "Result message",
 *   "ticketId": "123" (optional),
 *   "timestamp": 1234567890 (unix timestamp)
 * }
 *
 * CGI flow:
 * 1. Build JSON response object
 * 2. Set HTTP/CORS headers via checkMethodAndSetHeader()
 * 3. Output JSON to stdout (CGI body)
 * 4. Flush output for immediate delivery
 * 5. Log for debugging
 *
 * @param success True if operation succeeded, false on error
 * @param operation Operation identifier (e.g., "comment", "close_ticket")
 * @param message Result message for UI display
 * @param ticketId Optional ticket ID for ticket-related operations
 */
void WebInterface::sendActionResult(bool success, const std::string &operation,
                                    const std::string &message,
                                    const std::string &ticketId) {
  // Build response JSON with operation result
  nlohmann::json response;
  response["status"] = success ? "SUCCESS" : "ERROR";
  response["operation"] = operation;
  response["message"] = message;
  if (!ticketId.empty()) {
    response["ticketId"] = ticketId;
  }
  response["timestamp"] = time(nullptr);

  // Set proper CGI headers with CORS support
  const char *methodEnv = getenv("REQUEST_METHOD");
  std::string requestMethod = methodEnv ? methodEnv : "";
  bool check = checkMethodAndSetHeader(requestMethod);
  if (!check)
    return; // OPTIONS request, no body needed

  // Send JSON response to stdout (CGI body)
  std::cout << response.dump(2) << std::endl;
  std::cout.flush();

  // Log the response for debugging (separate from UI communication)
  logging::Logger::debug("Action result sent: " + response.dump());
}

//==============================================================================
// Plugin Factory Function (C Linkage)
//==============================================================================

/**
 * @brief Plugin factory function - creates WebInterface instance for
 * microkernel
 *
 * C linkage requirements:
 * - extern "C" prevents name mangling for dynamic library loading
 * - Microkernel uses dlsym() to locate this function by name
 * - Function signature must match Ui* (*)(nlohmann::json&)
 *
 * Plugin lifecycle:
 * 1. Microkernel loads libWebInterface.so via dlopen()
 * 2. Microkernel finds createUi() via dlsym()
 * 3. Microkernel calls createUi(config) to instantiate plugin
 * 4. Plugin pointer returned as Ui* base class
 * 5. Microkernel uses plugin via Ui interface methods
 *
 * @param config JSON configuration from config.json
 * @return Pointer to new WebInterface instance (as Ui* base class)
 */
extern "C" {
Ui *createUi(nlohmann::json &config) { return new WebInterface(config); }
}
