/**
 * @file TicketCreator.h
 * @brief Factory class for dynamic loading of Ticket model implementations
 *
 * This creator implements the Factory pattern with dynamic library loading
 * (dlopen/dlsym) to support the microkernel's plugin architecture. Unlike other
 * creators which load System plugins, TicketCreator loads Model
 * implementations, enabling different ticket representations (e.g., work
 * packages, issues, tickets from various systems).
 *
 * Plugin Loading Mechanism:
 * 1. Constructor(libPath) - Loads shared library (.so file) using dlopen()
 * 2. create(config, api) - Retrieves "createTicketFromDll" function pointer via
 * dlsym()
 * 3. Calls factory function to instantiate concrete Ticket model
 * 4. Returns pointer to Ticket instance for use in controllers
 *
 * Architecture Benefits:
 * - Zero recompilation for ticket format changes (swap .so files at runtime)
 * - Testable (can load mock implementations for unit tests)
 * - Configuration-driven (plugin path specified in config.json)
 * - Decouples ticket representation from business logic
 *
 * Difference from Other Creators:
 * - TicketCreator loads Model plugins (Ticket)
 * - Other creators load System plugins (TicketSystem, AddressSystem, Ui)
 * - TicketCreator's create() requires TicketSystem reference for API
 * interaction
 * - Constructor loads library immediately (no separate open() method)
 *
 * Error Handling:
 * - Throws std::runtime_error if library not found (dlopen fails)
 * - Throws std::runtime_error if factory function not found (dlsym fails)
 * - Automatically closes library in destructor (RAII pattern)
 *
 * @dependencies Models/Ticket.h (model interface), dlfcn.h (dynamic loading)
 * @see Ticket for model interface contract
 * @see Plugin implementations in lib/ folder for examples with
 * createTicketFromDll() export
 *
 */

#pragma once

#include "Models/Ticket.h"
#include <string>

/**
 * @class TicketCreator
 * @brief Factory for dynamically loading Ticket model plugins via dlopen/dlsym
 *
 * Manages the lifecycle of a dynamically loaded Ticket model plugin library.
 * Uses POSIX dynamic loading (dlopen) to load shared libraries at runtime and
 * retrieve factory functions (dlsym) that instantiate plugin implementations.
 *
 * Usage Pattern:
 * @code
 * TicketCreator creator("/path/to/libOpenProject.so");  // Constructor loads
 * library Ticket* ticket = creator.create(config, ticketSystemApi);  //
 * Instantiate model
 * // Use ticket...
 * delete ticket;  // Clean up model instance
 * // creator destructor automatically closes library
 * @endcode
 *
 * Plugin Requirements:
 * - Must export C-linkage function: extern "C" Ticket*
 * createTicketFromDll(nlohmann::json, TicketSystem&)
 * - Must implement Ticket interface (getId, getTitle, getStatus, etc.)
 * - Must be compiled as shared library (.so) with -fPIC flag
 * - Must handle JSON deserialization from ticket system API response
 *
 * Constructor vs open() Method:
 * - Unlike other creators, TicketCreator loads library in constructor
 * - No separate open() method needed
 * - Simpler API since library path is always required
 * - Fails fast if library cannot be loaded
 *
 * @note This class manages the .so file handle but NOT the created Ticket
 * instance. Caller is responsible for deleting the Ticket* returned by
 * create().
 */
class TicketCreator {
public:
  /**
   * @brief Constructor - loads the Ticket model plugin shared library
   *
   * Opens the specified shared library using dlopen() with RTLD_LAZY flag,
   * which defers symbol resolution until first use. This allows faster
   * instantiation of the creator.
   *
   * @param dllName Absolute or relative path to the plugin .so file
   *                Example: "/path/to/libOpenProject.so"
   *
   * @throws std::runtime_error If library cannot be loaded (file not found,
   *                            wrong architecture, missing dependencies)
   *                            Error message includes dlerror() details
   *
   * @note Uses RTLD_LAZY (lazy symbol resolution) for performance.
   *       Missing symbols will fail at create() time when dlsym() is called.
   */
  TicketCreator(const std::string &dllName);

  /**
   * @brief Destructor - automatically closes the loaded plugin library
   *
   * Uses dlclose() to unload the shared library and free resources.
   * Called automatically when creator goes out of scope (RAII pattern).
   *
   * @note Does NOT delete Ticket instances created by create().
   *       Caller must manage those pointers separately.
   */
  ~TicketCreator();

  /**
   * @brief Creates a Ticket instance using the loaded plugin's factory function
   *
   * Retrieves the "createTicketFromDll" function pointer from the loaded
   * library using dlsym(), then calls it to instantiate the concrete Ticket
   * model.
   *
   * Plugin Contract:
   * - Plugin must export: extern "C" Ticket*
   * createTicketFromDll(nlohmann::json, TicketSystem&)
   * - Function must return heap-allocated Ticket* (caller owns)
   * - Config parameter contains ticket data (from API response or local JSON)
   * - TicketSystem reference allows ticket to make API calls if needed
   *
   * @param config JSON configuration object containing ticket data
   *               Typically the JSON response from ticket system API (e.g.,
   * OpenProject work package)
   * @param api Reference to TicketSystem for API interaction
   *            Allows ticket model to fetch additional data or update itself
   *
   * @return Pointer to newly created Ticket instance (caller must delete)
   *
   * @throws std::runtime_error If "createTicketFromDll" symbol not found in
   * library Error message includes dlerror() details
   *
   * @note Caller is responsible for deleting the returned pointer
   * @note Config is passed by value (copy) unlike other creators
   */
  Ticket *create(nlohmann::json config, TicketSystem &api);

  /**
   * @brief Handle to the loaded shared library (managed by dlopen/dlclose)
   *
   * This pointer is set by constructor and used by create() to retrieve
   * symbols. Automatically cleaned up in destructor via dlclose().
   *
   * @warning Public for implementation simplicity - do not manipulate directly
   */
  void *dllPtr = nullptr;
};
