/**
 * @file main.cpp
 * @brief AID Microkernel - Main entry point and CGI request handler
 *
 * This file implements the core microkernel for the Agent Intelligence
 * Dashboard (AID). The system uses a plugin-based architecture with dynamically
 * loaded shared libraries and handles both CGI and direct execution modes.
 *
 * Architecture Overview:
 * ┌─────────────────────────────────────────────────────────────┐
 * │                        Microkernel                          │
 * ├─────────────────────────────────────────────────────────────┤
 * │  1. Load config.json                                        │
 * │  2. Initialize logger                                       │
 * │  3. Load plugins (AddressSystem, TicketSystem, UI)          │
 * │  4. Parse CGI environment (PATH_INFO)                       │
 * │  5. Route to appropriate controller                         │
 * │     - /ui/... → UiController (dashboard, tickets)           │
 * │     - /call/... → CallController (phone integration)        │
 * │  6. Controller processes request and outputs JSON           │
 * └─────────────────────────────────────────────────────────────┘
 *
 * Execution Modes:
 * 1. CGI Mode: Called by Apache with PATH_INFO environment variable
 *    - Input from stdin (POST data)
 *    - Output to stdout (HTTP headers + JSON)
 *
 * 2. Direct Mode: Called by SvelteKit API with command line args
 *    - Input from stdin or test file
 *    - Output to stdout (JSON only, no HTTP headers in some cases)
 *
 * Plugin System:
 * - Shared libraries loaded via dlopen() using Creator pattern
 * - Ticketsystem integration (ticketsystem.so) - Ticket management
 * - CardDAV server integration (cardDavServer.so) - Address/contact lookup
 * - WebInterface (libWebInterface.so) - UI bridge
 *
 * @see ServiceContainer.h for dependency injection system
 * @see Controllers/UiController.h for UI request handling
 * @see Controllers/CallController.h for call event handling
 */

#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "ConfigError.h"
#include "Constants.h"
#include "Controllers/CallController.h"
#include "Controllers/UiController.h"
#include "Creators/AddrSystemCreator.h"
#include "Creators/TicketCreator.h"
#include "Creators/TicketSystemCreator.h"
#include "Creators/UiCreator.h"
#include "Logger/Logger.h"
#include "Models/Call.h"
#include "Models/Ticket.h"
#include "ServiceContainer.h"
#include "Ui/Ui.h"

#include <poll.h>
#include <unistd.h>

// Global variables removed - using ServiceContainer for dependency injection

/**
 * @brief Load JSON configuration from file
 * @param configPath Path to config.json file
 * @param config Output parameter for loaded configuration
 * @throws ConfigError if config file cannot be read
 *
 * Loads the microkernel configuration containing:
 * - Plugin paths for shared libraries
 * - API credentials (Ticketsystem, CardDav)
 * - System settings and endpoints
 */
void loadConfig(const char *configPath, nlohmann::json &config) {
  std::ifstream cfg(configPath);
  if (!cfg.good()) {
    config = nlohmann::json({{"TicketSystem",
                              {
                                  {"lib", ""},
                              }}});
    throw ConfigError("Config leer");
  }
  cfg >> config;
}

/**
 * @brief Handle CORS preflight OPTIONS requests
 * @param requestMethod HTTP request method from CGI environment
 * @return true if backend processing needed, false if OPTIONS request handled
 *
 * Outputs CORS headers for browser preflight requests.
 * Returns false for OPTIONS requests (no further processing needed).
 */
bool checkOptions(std::string requestMethod) {
  // returns true if backend actions needed
  if (requestMethod == "OPTIONS") {
    std::cout << "Access-Control-Allow-Origin: *\n";
    std::cout << "Access-Control-Allow-Methods: POST, GET, OPTIONS\n";
    std::cout << "Access-Control-Allow-Headers: Content-Type\n";
    std::cout << "Content-Length: 0\n\n";
    return false;
  } else {
    std::cout << "Access-Control-Allow-Origin: *\n";
    std::cout << "Access-Control-Allow-Methods: POST, GET, OPTIONS\n";
    std::cout << "Access-Control-Allow-Headers: Content-Type\n";
    std::cout << "Content-Length: 0\n\n";
    return true;
  }
}

/**
 * @brief Extract URL path from CGI environment
 * @return URL path from PATH_INFO with leading slash removed
 *
 * Retrieves the PATH_INFO environment variable set by the web server
 * and normalizes it by removing the leading slash.
 * Used for routing requests to controllers.
 */
std::string getURLPath() {
  // Get the PATH_INFO environment variable
  const char *pathInfo = getenv("PATH_INFO");
  if (pathInfo == nullptr) {
    return "";
  }

  // Remove leading slash if present
  std::string path(pathInfo);
  if (path.size() > 0 && path[0] == '/') {
    path = path.substr(1);
  }

  return path;
}

/**
 * @brief Check if input is available on stdin without blocking
 * @return true if data is available to read, false otherwise
 *
 * Uses poll() with timeout to check stdin availability.
 * Prevents CGI process from hanging when no POST data is present.
 * Critical for direct execution mode where stdin may be empty.
 */
bool hasInputAvailable() {
  struct pollfd pfd;
  pfd.fd = STDIN_FILENO;
  pfd.events = POLLIN;

  // Non-blocking input check using poll() with timeout
  // Prevents CGI process from hanging when no POST data available
  int ret = poll(&pfd, 1, Constants::SystemConfig::POLL_TIMEOUT_MS);
  return (ret > 0 && (pfd.revents & POLLIN));
}

/**
 * @brief Read all available input from stdin in non-blocking mode
 * @return String containing all stdin data, or empty string if none available
 *
 * Reads POST data in chunks without blocking. Used for CGI input handling
 * where request body contains JSON payloads for ticket operations.
 * Accumulates all available stdin data into a single string.
 */
std::string readInputIfAvailable() {
  if (!hasInputAvailable()) {
    return "";
  }

  // Read POST data in chunks without blocking (for CGI input handling)
  // Accumulates all available stdin data into single string
  std::string input;
  char buffer[Constants::SystemConfig::INPUT_BUFFER_SIZE];
  while (hasInputAvailable()) {
    std::cin.read(buffer, sizeof(buffer));
    std::streamsize bytesRead = std::cin.gcount();
    if (bytesRead > 0) {
      input.append(buffer, bytesRead);
    } else {
      break;
    }
    std::cin.clear(); // Clear any error flags
  }
  return input;
}

/**
 * @brief Display usage information and exit
 * @param argv Command line argument vector
 * @return Exit code 10
 *
 * Shows proper command line usage for the microkernel.
 * Input file parameter is optional and only used for testing.
 */
int usage(char **argv) {
  std::cout << "usage " << argv[0] << " <config file> [<input file>]"
            << std::endl;
  std::cout << "<input file> is solely ment for testing purposes" << std::endl;
  return 10;
}

/**
 * @class Microkernel
 * @brief Core kernel managing plugin lifecycle and request routing
 *
 * The Microkernel class orchestrates the entire AID system:
 * 1. Configuration loading
 * 2. Plugin initialization (Address, Ticket, UI systems)
 * 3. Request routing to appropriate controllers
 *
 * Plugin Architecture:
 * - Uses Creator pattern for dynamic library loading
 * - Creators (AddrSystemCreator, TicketSystemCreator, UiCreator) manage
 * dlopen/dlclose
 * - Systems are loaded from paths specified in config.json
 * - Proper cleanup order ensures no vtable corruption
 *
 * Request Flow:
 * - Parse URL path from CGI or command line
 * - Create controller based on route (/ui/... or /call/...)
 * - Controller processes request and outputs JSON
 */
class Microkernel {
  nlohmann::json mConfig;
  AddrSystemCreator mAsc;
  TicketSystemCreator mTsc;
  UiCreator mUic;

  std::unique_ptr<AddressSystem> mAddressSystem;
  std::unique_ptr<TicketSystem> mTicketSystem;
  std::unique_ptr<Ui> mUi;

public:
  void init(const char *cfg) {
    // Configuration loading sequence - sets up all system components
    loadConfig(cfg, mConfig);
    logging::Logger::initialize(cfg);

    // Load AddressSystem plugin (DaviCal CardDAV integration)
    mAsc.open(mConfig["AddressSystem"]["libPath"]);
    mAddressSystem.reset(mAsc.create(mConfig["AddressSystem"]));

    // Load TicketSystem plugin (OpenProject API integration)
    logging::Logger::info("Starting ticket system");
    mTsc.open(mConfig["TicketSystem"]["libPath"]);
    mTicketSystem.reset(mTsc.create(mConfig["TicketSystem"]));

    logging::Logger::info("Ticket loaded continue main");

    // Load UI plugin (web interface bridge)
    mUic.open(mConfig["Ui"]["libPath"]);
    mUi.reset(mUic.create(mConfig["Ui"]));
    logging::Logger::info("Shared objects loaded");
  }

  /**
   * @brief Factory method: Creates appropriate controller based on URL route
   * @param url Request URL path from CGI or command line
   * @return Unique pointer to controller (UiController or CallController)
   * @throws std::runtime_error if URL doesn't match any known route
   *
   * Routing Table:
   * - /ui/... → UiController (dashboard, tickets, comments, close operations)
   * - /call/... → CallController (phone call events, CTI integration)
   */
  std::unique_ptr<Controller> createController(const std::string &url) {
    if (url.substr(0, Constants::Routes::UI_PREFIX_LENGTH) ==
        Constants::Routes::UI) {
      logging::Logger::debug("starting UiController");
      return std::make_unique<UiController>(*mTicketSystem, *mAddressSystem,
                                            *mUi);
    } else if (url.substr(0, Constants::Routes::CALL_PREFIX_LENGTH) ==
               Constants::Routes::CALL) {
      logging::Logger::debug("starting CallController");
      return std::make_unique<CallController>(*mTicketSystem, *mAddressSystem);
    }
    throw std::runtime_error("Invalid request, unknown controller " + url);
  }

  /**
   * @brief Execute a request through the appropriate controller
   * @param url Request URL path (routing key)
   * @param input Input stream containing request data (POST body)
   * @return Exit code (0 for success, non-zero for errors)
   *
   * Main request execution method that:
   * 1. Creates controller based on URL route
   * 2. Delegates request processing to controller
   * 3. Controller outputs JSON response to stdout
   */
  int run(std::string &url, std::istream &input) {
    logging::Logger::debug("Starting with kernel.run");
    int result = 0;

    // Debug log for testing purposes
    if (input.good()) {
      input.seekg(0);
      logging::Logger::debug("Input good ");
      std::string test;
      input >> test;
      logging::Logger::debug("url: " + url + " input: " + test);
    }

    // Create controller for the requested route and execute request
    std::unique_ptr<Controller> ctrl = createController(url);
    result = ctrl->run(input, url);

    return result;
  }
};

/**
 * @brief Initialize microkernel from command line arguments
 * @param argc Argument count
 * @param argv Argument vector (argv[1] must be config file path)
 * @param kernel Microkernel instance to initialize
 * @return true if initialization successful, false if argument validation fails
 *
 * Validates that config file path is provided and initializes the kernel.
 * Logs all command line arguments for debugging purposes.
 */
bool initMicrokernelFromArgs(int argc, char **argv, Microkernel &kernel) {
  if (argc < 2) {
    return false;
  }

  // Initialize with config file path from argv[1]
  kernel.init(argv[1]);

  // Log all arguments for debugging
  for (int i = 0; i < argc; ++i) {
    logging::Logger::debug("ARGV[" + std::to_string(i) +
                           "] = " + std::string(argv[i]));
  }
  logging::Logger::info("Config loaded");
  return true;
}

/**
 * @brief Extract URL from CGI environment and log it
 * @return URL path from PATH_INFO environment variable
 *
 * Gets PATH_INFO environment variable for routing purposes.
 * Used in CGI mode when Apache sets the environment.
 */
std::string getUrlFromEnvironment() {
  std::string url;
  if (const char *pathInfo = getenv("PATH_INFO"))
    url = pathInfo;
  logging::Logger::info("URL: " + url);
  return url;
}

/**
 * @brief Generate CGI error response for exceptions
 * @param e Exception to report
 *
 * Outputs error message in plain text format to stdout.
 * Used when the microkernel encounters fatal errors during request processing.
 */
void generateErrorResponse(const std::exception &e) {
  std::cout << "Content-Type: text/plain\n\n"
               "An Error occured:\n\n";
  std::cout << e.what() << std::endl;
}

/**
 * @brief Main entry point for AID microkernel
 * @param argc Argument count
 * @param argv Argument vector (argv[1] = config path, argv[2] = optional URL
 * override)
 * @return Exit code (0 = success, 10 = usage error, 500 = internal error)
 *
 * Execution Flow:
 * 1. Parse command line arguments
 * 2. Load configuration and initialize plugins
 * 3. Extract URL from CGI environment (PATH_INFO) or command line
 * 4. Route to appropriate controller and process request
 * 5. Output JSON response to stdout
 *
 * Error Handling:
 * - Catches all exceptions and outputs CGI-compatible error responses
 * - Returns HTTP 500 status code on errors
 */
int main(int argc, char **argv) {
  try {
    // Parse command line arguments and initialize microkernel
    Microkernel kernel;
    if (!initMicrokernelFromArgs(argc, argv, kernel))
      return usage(argv);

    // Extract URL from CGI environment and execute request
    std::string url = getUrlFromEnvironment();
    logging::Logger::info("Starting run from main bottom.");
    return kernel.run(url, std::cin);

  } catch (std::exception &e) {
    // Error handling for CGI - return plain text error response
    generateErrorResponse(e);
    return Constants::HttpStatus::INTERNAL_SERVER_ERROR;
  }
}
