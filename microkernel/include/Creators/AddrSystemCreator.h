/**
 * @file AddrSystemCreator.h
 * @brief Factory class for dynamic loading of AddressSystem plugin
 * implementations
 *
 * This creator implements the Factory pattern with dynamic library loading
 * (dlopen/dlsym) to support the microkernel's plugin architecture. It enables
 * runtime selection of address system implementations (e.g., CardDAV, LDAP,
 * ActiveDirectory) without recompilation of the core system.
 *
 * Plugin Loading Mechanism:
 * 1. open(libPath) - Loads shared library (.so file) using dlopen()
 * 2. create(config) - Retrieves "createAddressSystem" function pointer via
 * dlsym()
 * 3. Calls factory function to instantiate concrete AddressSystem
 * implementation
 * 4. Returns pointer to AddressSystem interface for dependency injection
 *
 * Architecture Benefits:
 * - Zero recompilation for system changes (swap .so files at runtime)
 * - Testable (can load mock implementations for unit tests)
 * - Configuration-driven (plugin path specified in config.json)
 *
 * Error Handling:
 * - Throws std::runtime_error if library not found (dlopen fails)
 * - Throws std::runtime_error if factory function not found (dlsym fails)
 * - Automatically closes library in destructor (RAII pattern)
 *
 * @dependencies Systems/AddressSystem.h (interface), dlfcn.h (dynamic loading)
 * @see AddressSystem for plugin interface contract
 * @see Plugin implementations in lib/ folder for examples
 *
 */

#pragma once

#include "Systems/AddressSystem.h"
#include "json.hpp"
#include <string>

/**
 * @class AddrSystemCreator
 * @brief Factory for dynamically loading AddressSystem plugins via dlopen/dlsym
 *
 * Manages the lifecycle of a dynamically loaded AddressSystem plugin library.
 * Uses POSIX dynamic loading (dlopen) to load shared libraries at runtime and
 * retrieve factory functions (dlsym) that instantiate plugin implementations.
 *
 * Usage Pattern:
 * @code
 * AddrSystemCreator creator;
 * creator.open("/path/to/libDaviCal.so");  // Load plugin library
 * AddressSystem* system = creator.create(config);  // Instantiate plugin
 * // Use system...
 * delete system;  // Clean up plugin instance
 * // creator destructor automatically closes library
 * @endcode
 *
 * Plugin Requirements:
 * - Must export C-linkage function: extern "C" AddressSystem*
 * createAddressSystem(nlohmann::json&)
 * - Must implement AddressSystem interface (getInformationByNumber,
 * getInformationByName, etc.)
 * - Must be compiled as shared library (.so) with -fPIC flag
 *
 * @note This class manages the .so file handle but NOT the created
 * AddressSystem instance. Caller is responsible for deleting the AddressSystem*
 * returned by create().
 */
class AddrSystemCreator {
public:
  /**
   * @brief Destructor - automatically closes the loaded plugin library
   *
   * Uses dlclose() to unload the shared library and free resources.
   * Called automatically when creator goes out of scope (RAII pattern).
   *
   * @note Does NOT delete AddressSystem instances created by create().
   *       Caller must manage those pointers separately.
   */
  ~AddrSystemCreator();

  /**
   * @brief Loads the AddressSystem plugin shared library (.so file)
   *
   * Opens the specified shared library using dlopen() with RTLD_NOW flag,
   * which resolves all symbols immediately and fails fast if dependencies
   * are missing.
   *
   * @param dllName Absolute or relative path to the plugin .so file
   *                Example: "/path/to/libDaviCal.so"
   *
   * @throws std::runtime_error If library cannot be loaded (file not found,
   *                            wrong architecture, missing dependencies)
   *                            Error message includes dlerror() details
   *
   * @note Must be called before create(). Repeated calls will leak handles
   *       (current implementation does not check if already open).
   */
  void open(const std::string &dllName);

  /**
   * @brief Creates an AddressSystem instance using the loaded plugin's factory
   * function
   *
   * Retrieves the "createAddressSystem" function pointer from the loaded
   * library using dlsym(), then calls it to instantiate the concrete
   * AddressSystem implementation.
   *
   * Plugin Contract:
   * - Plugin must export: extern "C" AddressSystem*
   * createAddressSystem(nlohmann::json& config)
   * - Function must return heap-allocated AddressSystem* (caller owns)
   * - Config parameter is passed directly to plugin for initialization
   *
   * @param config JSON configuration object (typically from config.json
   * AddressSystem section) Contains plugin-specific settings (API URLs,
   * credentials, etc.)
   *
   * @return Pointer to newly created AddressSystem instance (caller must
   * delete)
   *
   * @throws std::runtime_error If "createAddressSystem" symbol not found in
   * library Error message includes dlerror() details
   *
   * @pre open() must have been called successfully
   *
   * @note Caller is responsible for deleting the returned pointer
   */
  AddressSystem *create(nlohmann::json &config);

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
