/**
 * @file Controller.h
 * @brief Base controller class for handling HTTP requests in the AID
 * microkernel system
 *
 * This abstract class provides the foundation for all controller
 * implementations (UiController, CallController). Controllers process incoming
 * HTTP requests with JSON payloads and URL parameters, implementing specific
 * business logic for different system functions.
 */

#pragma once
#include "json.hpp"
#include <string>

using namespace std;

/**
 * @class Controller
 * @brief Abstract base class for all request handlers in the system
 *
 * Controllers serve as the main entry point for processing HTTP requests from
 * external systems (SvelteKit UI, phone system webhooks). Each controller
 * implementation handles a specific domain (UI actions, call events).
 *
 * The controller pattern provides:
 * - Unified interface for request processing
 * - Separation of concerns between routing and business logic
 * - Dependency injection through constructor parameters
 */
class Controller {
public:
  Controller();
  virtual ~Controller();

  /**
   * @brief Process an incoming request with payload and URL parameters
   *
   * @param payload Input stream containing the request body (typically JSON)
   * @param urlParams URL path and query parameters (e.g.,
   * "/ui/dashboard/username")
   * @return HTTP status code (0 = error, 1+ = success, or Constants::HttpStatus
   * values)
   *
   * Pure virtual method that must be implemented by derived classes to handle
   * specific request types. The implementation should:
   * 1. Parse the payload (usually JSON)
   * 2. Route to appropriate handler based on urlParams
   * 3. Execute business logic
   * 4. Return success/error status
   */
  virtual int run(std::istream &payload, std::string &urlParams) = 0;
};
