/**
 * @file UiCreator.h
 * @brief Factory class for dynamic loading of UI plugin implementations
 *
 * This creator implements the Factory pattern with dynamic library loading
 * (dlopen/dlsym) to support the microkernel's plugin architecture. It enables
 * runtime selection of UI implementations (e.g., WebInterface, CLI, REST API)
 * without recompilation of the core system.
 *
 * Plugin Loading Mechanism:
 * 1. open(libPath) - Loads shared library (.so file) using dlopen()
 * 2. create(config) - Retrieves "createUi" function pointer via dlsym()
 * 3. Calls factory function to instantiate concrete Ui implementation
 * 4. Returns pointer to Ui interface for dependency injection
 *
 * Architecture Benefits:
 * - Zero recompilation for UI changes (swap .so files at runtime)
 * - Testable (can load mock implementations for unit tests)
 * - Configuration-driven (plugin path specified in config.json)
 * - Multiple UI frontends possible (web, CLI, API simultaneously)
 *
 * Error Handling:
 * - Throws std::runtime_error if library not found (dlopen fails)
 * - Throws std::runtime_error if factory function not found (dlsym fails)
 * - Automatically closes library in destructor (RAII pattern)
 *
 * @dependencies Ui/Ui.h (interface), dlfcn.h (dynamic loading)
 * @see Ui for plugin interface contract
 * @see WebInterface.cpp for example plugin implementation
 *
 */

#pragma once

#include "Ui/Ui.h"
#include <string>

/**
 * @class UiCreator
 * @brief Factory for dynamically loading UI plugins via dlopen/dlsym
 *
 * Manages the lifecycle of a dynamically loaded UI plugin library.
 * Uses POSIX dynamic loading (dlopen) to load shared libraries at runtime and
 * retrieve factory functions (dlsym) that instantiate plugin implementations.
 *
 * Usage Pattern:
 * @code
 * UiCreator creator;
 * creator.open("/path/to/libWebInterface.so");  // Load plugin library
 * Ui* ui = creator.create(config);  // Instantiate plugin
 * // Use ui...
 * delete ui;  // Clean up plugin instance
 * // creator destructor automatically closes library
 * @endcode
 *
 * Plugin Requirements:
 * - Must export C-linkage function: extern "C" Ui* createUi(nlohmann::json&)
 * - Must implement Ui interface (handleRequest, etc.)
 * - Must be compiled as shared library (.so) with -fPIC flag
 * - Must handle its own HTTP/CGI environment setup
 *
 * @note This class manages the .so file handle but NOT the created Ui instance.
 *       Caller is responsible for deleting the Ui* returned by create().
 */
class UiCreator {
public:
  /**
   * @brief Loads the UI plugin shared library (.so file)
   *
   * Opens the specified shared library using dlopen() with RTLD_NOW flag,
   * which resolves all symbols immediately. This fail-fast approach catches
   * missing dependencies at load time rather than at first use.
   *
   * @param dllName Absolute or relative path to the plugin .so file
   *                Example: "/path/to/libWebInterface.so"
   *
   * @throws std::runtime_error If library cannot be loaded (file not found,
   *                            wrong architecture, missing dependencies)
   *                            Error message includes dlerror() details
   *
   * @note Uses RTLD_NOW (immediate symbol resolution) for fail-fast behavior.
   *       Missing symbols will be detected immediately at open() time.
   * @note Must be called before create(). Repeated calls will leak handles.
   */
  void open(const std::string &dllName);

  /**
   * @brief Destructor - automatically closes the loaded plugin library
   *
   * Uses dlclose() to unload the shared library and free resources.
   * Called automatically when creator goes out of scope (RAII pattern).
   *
   * @note Does NOT delete Ui instances created by create().
   *       Caller must manage those pointers separately.
   */
  ~UiCreator();

  /**
   * @brief Creates a Ui instance using the loaded plugin's factory function
   *
   * Retrieves the "createUi" function pointer from the loaded library
   * using dlsym(), then calls it to instantiate the concrete Ui implementation.
   *
   * Plugin Contract:
   * - Plugin must export: extern "C" Ui* createUi(nlohmann::json& config)
   * - Function must return heap-allocated Ui* (caller owns)
   * - Config parameter is passed directly to plugin for initialization
   *
   * @param config JSON configuration object (typically from config.json Ui
   * section) Contains plugin-specific settings (port, routes, etc.)
   *
   * @return Pointer to newly created Ui instance (caller must delete)
   *
   * @throws std::runtime_error If "createUi" symbol not found in library
   *                            Error message includes dlerror() details
   *
   * @pre open() must have been called successfully
   *
   * @note Caller is responsible for deleting the returned pointer
   */
  Ui *create(nlohmann::json &config);

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
