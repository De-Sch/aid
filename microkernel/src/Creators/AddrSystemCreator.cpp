/**
 * @file AddrSystemCreator.cpp
 * @brief Implementation of AddressSystem plugin factory with dynamic library
 * loading
 *
 * This file implements the factory pattern using POSIX dynamic loading
 * (dlopen/dlsym) to load AddressSystem plugins at runtime. This is the core of
 * the microkernel's plugin architecture for address system implementations.
 *
 * Dynamic Loading Process:
 * 1. dlopen(libPath, RTLD_NOW) - Load shared library, resolve all symbols
 * immediately
 * 2. dlsym(handle, "createAddressSystem") - Retrieve factory function pointer
 * 3. Call factory function with config JSON to instantiate plugin
 * 4. Return AddressSystem* interface pointer for dependency injection
 * 5. dlclose(handle) - Unload library when creator is destroyed
 *
 * Error Handling Strategy:
 * - Fail fast: RTLD_NOW ensures all symbols resolve at load time
 * - Exceptions: All errors throw std::runtime_error with dlerror() details
 * - RAII: Destructor ensures dlclose() is called even if exceptions occur
 *
 * @dependencies dlfcn.h (POSIX dynamic loading), Systems/AddressSystem.h
 * (interface)
 * @see AddrSystemCreator.h for class documentation
 * @see Plugin implementations in lib/ folder for examples with
 * createAddressSystem() export
 *
 */

#include "Creators/AddrSystemCreator.h"

#include "Systems/AddressSystem.h"
#include <dlfcn.h>
#include <iostream>
#include <stdexcept>
#include <string>

/**
 * @brief Loads the AddressSystem plugin shared library using dlopen()
 *
 * Opens the specified shared library with RTLD_NOW flag, which resolves all
 * symbols immediately. This fail-fast approach catches missing dependencies
 * at load time rather than at first use.
 *
 * Implementation Details:
 * - Uses dlopen() from dlfcn.h (POSIX dynamic loading API)
 * - RTLD_NOW flag: Resolve all undefined symbols before dlopen() returns
 * - Stores handle in dllPtr member for use by create()
 *
 * @param dllName Path to shared library (absolute or relative)
 *
 * @throws std::runtime_error If dlopen() fails (library not found, wrong
 * architecture, missing symbol dependencies, or permission denied) Error
 * message format: "not found: <dlerror details>"
 *
 * @note This method does not validate if dllPtr is already set. Calling open()
 *       multiple times without destroy will leak library handles.
 */
void AddrSystemCreator::open(const std::string &dllName) {
  // Attempt to load the shared library with immediate symbol resolution
  // RTLD_NOW: All undefined symbols in the library are resolved before dlopen()
  // returns If resolution fails, dlopen() returns NULL and we throw an
  // exception
  dllPtr = dlopen(dllName.c_str(), RTLD_NOW);
  if (!dllPtr) {
    // dlerror() returns readable string describing the error
    // Common errors: File not found, architecture mismatch, missing
    // dependencies
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
 * @note This does NOT delete AddressSystem instances created by create().
 *       Those pointers are owned by the caller and must be deleted separately
 *       before this destructor is called.
 *
 * @warning If dllPtr is nullptr (open() was never called or failed), dlclose()
 *          behavior is undefined. Current implementation does not check this.
 */
AddrSystemCreator::~AddrSystemCreator() {
  // Unload the shared library and free resources
  // Note: This does not affect AddressSystem instances created by create()
  // Caller must delete those separately before destroying the creator
  if (dllPtr) {
    dlclose(dllPtr);
  }
}

/**
 * @brief Instantiates an AddressSystem plugin using the loaded library's
 * factory function
 *
 * Uses dlsym() to retrieve the "createAddressSystem" function pointer from the
 * loaded library, then calls it to create a concrete AddressSystem
 * implementation.
 *
 * Plugin Contract (enforced by dlsym lookup):
 * - Plugin must export: extern "C" AddressSystem*
 * createAddressSystem(nlohmann::json&)
 * - extern "C" prevents C++ name mangling, ensuring symbol is findable
 * - Function must allocate AddressSystem on heap and return pointer
 * - Caller owns the returned pointer (must delete when done)
 *
 * Implementation Details:
 * - dlsym() searches for symbol "createAddressSystem" in loaded library
 * - reinterpret_cast converts void* to function pointer (AddrSysCreator* type)
 * - Function is called immediately with config parameter
 *
 * @param config JSON configuration object from config.json AddressSystem
 * section Passed directly to plugin for initialization (API URLs, credentials,
 * etc.)
 *
 * @return Heap-allocated AddressSystem* (caller must delete)
 *
 * @throws std::runtime_error If "createAddressSystem" symbol not found in
 * library Error message format: "function not found: <dlerror details>"
 *
 * @pre open() must have been called successfully (dllPtr must be valid)
 *
 * @note Repeated calls create multiple independent AddressSystem instances
 */
AddressSystem *AddrSystemCreator::create(nlohmann::json &config) {
  // Retrieve the factory function pointer from the loaded library
  // Symbol name "createAddressSystem" must match the extern "C" export in
  // plugin reinterpret_cast: dlsym returns void*, we cast to function pointer
  // type
  auto creatorFunction =
      reinterpret_cast<AddrSysCreator *>(dlsym(dllPtr, "createAddressSystem"));

  if (!creatorFunction) {
    // Symbol not found - plugin is missing the required factory function
    // Common cause: Plugin did not use extern "C" or function has wrong
    // signature
    throw std::runtime_error(std::string("function not found: ") + dlerror());
  }

  // Call the factory function to instantiate the concrete AddressSystem
  // implementation Config is passed by reference to avoid copy (plugin may
  // modify it for defaults) Returned pointer is owned by caller (must delete
  // when done)
  return (*creatorFunction)(config);
}
