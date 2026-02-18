/**
 * @file TicketSystemCreator.cpp
 * @brief Implementation of TicketSystem plugin factory with dynamic library
 * loading
 *
 * This file implements the factory pattern using POSIX dynamic loading
 * (dlopen/dlsym) to load TicketSystem plugins at runtime. This is the core of
 * the microkernel's plugin architecture for ticket system implementations.
 *
 * Dynamic Loading Process:
 * 1. dlopen(libPath, RTLD_LAZY) - Load shared library, defer symbol resolution
 * 2. dlsym(handle, "createTicketSystem") - Retrieve factory function pointer
 * 3. Call factory function with config JSON to instantiate plugin
 * 4. Return TicketSystem* interface pointer for dependency injection
 * 5. dlclose(handle) - Unload library when creator is destroyed
 *
 * RTLD_LAZY vs RTLD_NOW:
 * - RTLD_LAZY: Defers symbol resolution until first use (faster startup)
 * - RTLD_NOW: Resolves all symbols immediately (fail-fast, but slower)
 * - This implementation uses RTLD_LAZY for performance
 * - Missing symbols will cause runtime error when first called
 *
 * Logging Strategy:
 * - INFO: Library path being loaded (successful open operations)
 * - DEBUG: Symbol resolution steps (dlsym, instance creation)
 * - ERROR: All failure cases (dlopen fails, dlsym fails)
 * - Uses Logger singleton for centralized log management
 *
 * Error Handling Strategy:
 * - Exceptions: All errors throw std::runtime_error with dlerror() details
 * - RAII: Destructor ensures dlclose() is called even if exceptions occur
 * - Logging: All errors logged before throwing for debugging
 *
 * @dependencies dlfcn.h (POSIX dynamic loading), Systems/TicketSystem.h
 * (interface), Logger/Logger.h (logging singleton)
 * @see TicketSystemCreator.h for class documentation
 * @see Plugin implementations in lib/ folder for examples with
 * createTicketSystem() export
 */

#include "Creators/TicketSystemCreator.h"

#include "Logger/Logger.h"
#include "Systems/TicketSystem.h"
#include <dlfcn.h>
#include <iostream>
#include <stdexcept>
#include <string>

/**
 * @brief Loads the TicketSystem plugin shared library using dlopen()
 *
 * Opens the specified shared library with RTLD_LAZY flag, which defers symbol
 * resolution until first use. This improves startup time but may delay
 * detection of missing dependencies until the symbol is actually needed.
 *
 * Implementation Details:
 * - Uses dlopen() from dlfcn.h (POSIX dynamic loading API)
 * - RTLD_LAZY flag: Defer undefined symbol resolution until first use
 * - Stores handle in dllPtr member for use by create()
 * - Logs the library path before attempting load (INFO level)
 * - Logs errors if load fails (ERROR level)
 *
 * RTLD_LAZY vs RTLD_NOW Trade-off:
 * - RTLD_LAZY: Faster startup, deferred error detection
 * - RTLD_NOW: Slower startup, immediate error detection (fail-fast)
 * - Note: Commented-out RTLD_NOW code left for reference (line 10 of original)
 *
 * @param dllName Path to shared library (absolute or relative)
 *                Example:
 * "/home/user/projects/aid/build/microkernel/libOpenProject.so"
 *
 * @throws std::runtime_error If dlopen() fails (library not found, wrong
 * architecture, missing symbol dependencies, or permission denied) Error
 * message format: "not found: <dlerror details>"
 *
 * @note This method does not validate if dllPtr is already set. Calling open()
 *       multiple times without destroy will leak library handles.
 */
void TicketSystemCreator::open(const std::string &dllName) {
  // Log the library path being loaded (for debugging and audit trail)
  logging::Logger::info("TicketSystemCreator: Loading DLL: " + dllName);

  // Attempt to load the shared library with lazy symbol resolution
  // RTLD_LAZY: Defer undefined symbol resolution until dlsym() or first use
  // This improves startup performance but may delay error detection
  // Note: Original code had RTLD_NOW commented out - left as reference
  // if(!(dllPtr = dlopen(dllName.c_str(), RTLD_NOW)))
  dllPtr = dlopen(dllName.c_str(), RTLD_LAZY);

  if (!dllPtr) {
    // Library load failed - log error and throw exception
    // dlerror() returns readable string describing the error
    // Common errors: File not found, architecture mismatch, missing
    // dependencies
    logging::Logger::error("TicketSystemCreator: Failed to load DLL: " +
                           dllName);
    throw std::runtime_error(std::string("not found: ") + dlerror());
  }
}

/**
 * @brief Destructor - unloads the plugin library and frees resources
 *
 * Calls dlclose() to decrement the reference count on the loaded library.
 * When count reaches zero, the library is unloaded from memory.
 *
 * RAII Pattern:
 * - Constructor equivalent is open()
 * - Destructor ensures cleanup even if exceptions occur
 * - Automatic cleanup when creator goes out of scope
 *
 * @note This does NOT delete TicketSystem instances created by create().
 *       Those pointers are owned by the caller and must be deleted separately
 *       before this destructor is called.
 *
 * @warning If dllPtr is nullptr (open() was never called or failed), dlclose()
 *          behavior is undefined. Current implementation does not check this.
 */
TicketSystemCreator::~TicketSystemCreator() {
  // Unload the shared library and free resources
  // Note: This does not affect TicketSystem instances created by create()
  // Caller must delete those separately before destroying the creator
  if (dllPtr) {
    dlclose(dllPtr);
  }
}

/**
 * @brief Instantiates a TicketSystem plugin using the loaded library's factory
 * function
 *
 * Uses dlsym() to retrieve the "createTicketSystem" function pointer from the
 * loaded library, then calls it to create a concrete TicketSystem
 * implementation.
 *
 * Plugin Contract (enforced by dlsym lookup):
 * - Plugin must export: extern "C" TicketSystem*
 * createTicketSystem(nlohmann::json&)
 * - extern "C" prevents C++ name mangling, ensuring symbol is findable
 * - Function must allocate TicketSystem on heap and return pointer
 * - Caller owns the returned pointer (must delete when done)
 *
 * Implementation Details:
 * - dlsym() searches for symbol "createTicketSystem" in loaded library
 * - reinterpret_cast converts void* to function pointer (TicketSysCreator*
 * type)
 * - Function is called immediately with config parameter
 * - Logs debug information at each step for troubleshooting
 *
 * Logging Trace:
 * 1. DEBUG: "Loading symbol 'createTicketSystem'" - before dlsym()
 * 2. DEBUG: "Symbol loaded, creating instance" - after dlsym(), before function
 * call
 * 3. ERROR: "Symbol 'createTicketSystem' not found" - if dlsym() fails
 *
 * @param config JSON configuration object from config.json TicketSystem section
 *               Passed directly to plugin for initialization (API URLs,
 * credentials, etc.)
 *
 * @return Heap-allocated TicketSystem* (caller must delete)
 *
 * @throws std::runtime_error If "createTicketSystem" symbol not found in
 * library Error message format: "function not found: <dlerror details>"
 *
 * @pre open() must have been called successfully (dllPtr must be valid)
 *
 * @note Repeated calls create multiple independent TicketSystem instances
 */
TicketSystem *TicketSystemCreator::create(nlohmann::json &config) {
  // Log the start of symbol loading for debugging
  logging::Logger::debug(
      "TicketSystemCreator::create() - Loading symbol 'createTicketSystem'");

  // Retrieve the factory function pointer from the loaded library
  // Symbol name "createTicketSystem" must match the extern "C" export in plugin
  // reinterpret_cast: dlsym returns void*, we cast to function pointer type
  auto creatorFunction =
      reinterpret_cast<TicketSysCreator *>(dlsym(dllPtr, "createTicketSystem"));

  // Log successful symbol resolution
  logging::Logger::debug(
      "TicketSystemCreator::create() - Symbol loaded, creating instance");

  if (!creatorFunction) {
    // Symbol not found - plugin is missing the required factory function
    // Common cause: Plugin did not use extern "C" or function has wrong
    // signature
    logging::Logger::error(
        "TicketSystemCreator: Symbol 'createTicketSystem' not found in DLL");
    throw std::runtime_error(std::string("function not found: ") + dlerror());
  }

  // Call the factory function to instantiate the concrete TicketSystem
  // implementation Config is passed by reference to avoid copy (plugin may
  // modify it for defaults) Returned pointer is owned by caller (must delete
  // when done)
  return (*creatorFunction)(config);
}
