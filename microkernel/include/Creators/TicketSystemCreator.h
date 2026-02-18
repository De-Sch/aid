/**
 * @file TicketSystemCreator.h
 * @brief Factory class for dynamic loading of TicketSystem plugin
 * implementations
 *
 * This creator implements the Factory pattern with dynamic library loading
 * (dlopen/dlsym) to support the microkernel's plugin architecture. It enables
 * runtime selection of ticket system implementations (e.g., OpenProject, Jira,
 * Redmine, GitHub Issues) without recompilation of the core system.
 *
 * Plugin Loading Mechanism:
 * 1. open(libPath) - Loads shared library (.so file) using dlopen()
 * 2. create(config) - Retrieves "createTicketSystem" function pointer via
 * dlsym()
 * 3. Calls factory function to instantiate concrete TicketSystem implementation
 * 4. Returns pointer to TicketSystem interface for dependency injection
 *
 * Architecture Benefits:
 * - Zero recompilation for system changes (swap .so files at runtime)
 * - Testable (can load mock implementations for unit tests)
 * - Configuration-driven (plugin path specified in config.json)
 * - Isolated failures (plugin errors don't crash microkernel)
 *
 * Error Handling:
 * - Throws std::runtime_error if library not found (dlopen fails)
 * - Throws std::runtime_error if factory function not found (dlsym fails)
 * - Logs all operations and errors using Logger singleton
 * - Automatically closes library in destructor (RAII pattern)
 *
 * @dependencies Systems/TicketSystem.h (interface), dlfcn.h (dynamic loading)
 * @see TicketSystem for plugin interface contract
 * @see Plugin implementations in lib/ folder for examples
 *
 */

#pragma once

#include "Systems/TicketSystem.h"
#include <string>

/**
 * @class TicketSystemCreator
 * @brief Factory for dynamically loading TicketSystem plugins via dlopen/dlsym
 *
 * Manages the lifecycle of a dynamically loaded TicketSystem plugin library.
 * Uses POSIX dynamic loading (dlopen) to load shared libraries at runtime and
 * retrieve factory functions (dlsym) that instantiate plugin implementations.
 *
 * Usage Pattern:
 * @code
 * TicketSystemCreator creator;
 * creator.open("/path/to/libOpenProject.so");  // Load plugin library
 * TicketSystem* system = creator.create(config);  // Instantiate plugin
 * // Use system...
 * delete system;  // Clean up plugin instance
 * // creator destructor automatically closes library
 * @endcode
 *
 * Plugin Requirements:
 * - Must export C-linkage function: extern "C" TicketSystem*
 * createTicketSystem(nlohmann::json&)
 * - Must implement TicketSystem interface (createTicket, updateTicket,
 * getTicket, etc.)
 * - Must be compiled as shared library (.so) with -fPIC flag
 * - Must handle its own configuration validation and error handling
 *
 * Logging Integration:
 * - Uses Logger singleton for tracing plugin loading operations
 * - Logs INFO level: Library path being loaded
 * - Logs DEBUG level: Symbol resolution steps
 * - Logs ERROR level: Load failures with dlerror() details
 *
 * @note This class manages the .so file handle but NOT the created TicketSystem
 * instance. Caller is responsible for deleting the TicketSystem* returned by
 * create().
 */
class TicketSystemCreator {
public:
  /**
   * @brief Loads the TicketSystem plugin shared library (.so file)
   *
   * Opens the specified shared library using dlopen() with RTLD_LAZY flag,
   * which defers symbol resolution until first use. This allows faster
   * startup but may delay detection of missing dependencies.
   *
   * @param dllName Absolute or relative path to the plugin .so file
   *                Example: "/path/to/libOpenProject.so"
   *
   * @throws std::runtime_error If library cannot be loaded (file not found,
   *                            wrong architecture, missing dependencies)
   *                            Error message includes dlerror() details
   *
   * @note Uses RTLD_LAZY (lazy symbol resolution) vs RTLD_NOW (immediate
   * resolution) to improve startup time. Missing symbols will fail at first
   * use.
   * @note Must be called before create(). Repeated calls will leak handles.
   */
  void open(const std::string &dllName);

  /**
   * @brief Destructor - automatically closes the loaded plugin library
   *
   * Uses dlclose() to unload the shared library and free resources.
   * Called automatically when creator goes out of scope (RAII pattern).
   *
   * @note Does NOT delete TicketSystem instances created by create().
   *       Caller must manage those pointers separately.
   */
  ~TicketSystemCreator();

  /**
   * @brief Creates a TicketSystem instance using the loaded plugin's factory
   * function
   *
   * Retrieves the "createTicketSystem" function pointer from the loaded library
   * using dlsym(), then calls it to instantiate the concrete TicketSystem
   * implementation.
   *
   * Plugin Contract:
   * - Plugin must export: extern "C" TicketSystem*
   * createTicketSystem(nlohmann::json& config)
   * - Function must return heap-allocated TicketSystem* (caller owns)
   * - Config parameter is passed directly to plugin for initialization
   *
   * @param config JSON configuration object (typically from config.json
   * TicketSystem section) Contains plugin-specific settings (API URLs,
   * credentials, project IDs, etc.)
   *
   * @return Pointer to newly created TicketSystem instance (caller must delete)
   *
   * @throws std::runtime_error If "createTicketSystem" symbol not found in
   * library Error message includes dlerror() details
   *
   * @pre open() must have been called successfully
   *
   * @note Caller is responsible for deleting the returned pointer
   * @note Logs debug information about symbol loading and instance creation
   */
  TicketSystem *create(nlohmann::json &config);

  /**
   * @brief Handle to the loaded shared library (managed by dlopen/dlclose)
   *
   * This pointer is set by open() and used by create() to retrieve symbols.
   * Automatically cleaned up in destructor via dlclose().
   *
   * @warning Public for implementation simplicity - do not manipulate directly
   */
  void *dllPtr = nullptr;
};
