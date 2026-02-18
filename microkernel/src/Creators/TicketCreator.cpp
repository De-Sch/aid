/**
 * @file TicketCreator.cpp
 * @brief Implementation of Ticket model factory with dynamic library loading
 *
 * This file implements the factory pattern using POSIX dynamic loading
 * (dlopen/dlsym) to load Ticket model plugins at runtime. Unlike other creators
 * which load System plugins, this creator loads Model plugins for ticket
 * representation.
 *
 * Dynamic Loading Process:
 * 1. Constructor: dlopen(libPath, RTLD_LAZY) - Load shared library in
 * constructor
 * 2. create(): dlsym(handle, "createTicketFromDll") - Retrieve factory function
 * 3. Call factory function with config JSON and TicketSystem reference
 * 4. Return Ticket* model pointer for use in controllers
 * 5. Destructor: dlclose(handle) - Unload library when creator is destroyed
 *
 * Model vs System Plugin:
 * - Model plugins: Represent data structures (Ticket, Call)
 * - System plugins: Provide APIs and business logic (TicketSystem,
 * AddressSystem)
 * - TicketCreator creates Models, not Systems
 * - Models need System reference to interact with external APIs
 *
 * RTLD_LAZY Strategy:
 * - Uses RTLD_LAZY for faster creator instantiation
 * - Symbol resolution deferred to first create() call
 * - Acceptable since ticket creation is not on critical startup path
 * - Contrasts with UiCreator which uses RTLD_NOW for reliability
 *
 * Constructor Loading:
 * - Library loaded in constructor, not separate open() method
 * - Simpler API - library path always required
 * - Fail-fast behavior if library missing
 * - Different pattern from other creators (TicketSystemCreator,
 * AddrSystemCreator, UiCreator)
 *
 * @dependencies dlfcn.h (POSIX dynamic loading), Models/Ticket.h (model
 * interface), Logger/Logger.h (logging singleton)
 * @see TicketCreator.h for class documentation
 * @see Plugin implementations in lib/ folder for examples with
 * createTicketFromDll() export
 *
 */

#include "Creators/TicketCreator.h"

#include "Logger/Logger.h"
#include "Models/Ticket.h"
#include <dlfcn.h>
#include <iostream>
#include <stdexcept>
#include <string>

/**
 * @brief Constructor - loads the Ticket model plugin shared library
 *
 * Opens the specified shared library using dlopen() with RTLD_LAZY flag.
 * This is called immediately when TicketCreator is instantiated, unlike
 * other creators which have a separate open() method.
 *
 * Implementation Details:
 * - Uses dlopen() from dlfcn.h (POSIX dynamic loading API)
 * - RTLD_LAZY flag: Defer undefined symbol resolution until first use
 * - Stores handle in dllPtr member for use by create()
 * - Throws exception immediately if library cannot be loaded
 *
 * Constructor vs open() Method:
 * - Other creators: creator.open(path) - two-step initialization
 * - TicketCreator: TicketCreator(path) - single-step initialization
 * - Rationale: Ticket model plugin path is always known at creator
 * instantiation
 * - Simpler API, but less flexible than two-step approach
 *
 * @param dllName Path to shared library (absolute or relative)
 *
 * @throws std::runtime_error If dlopen() fails (library not found, wrong
 * architecture, missing symbol dependencies, or permission denied) Error
 * message format: "not found: <dlerror details>"
 */
TicketCreator::TicketCreator(const std::string &dllName) {
  // Attempt to load the shared library with lazy symbol resolution
  // RTLD_LAZY: Defer undefined symbol resolution until dlsym() or first use
  // This improves creator instantiation performance
  dllPtr = dlopen(dllName.c_str(), RTLD_LAZY);

  if (!dllPtr) {
    // Library load failed - throw exception immediately
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
 * - Constructor loads library
 * - Destructor ensures cleanup even if exceptions occur
 * - Automatic cleanup when creator goes out of scope
 *
 * @note This does NOT delete Ticket instances created by create().
 *       Those pointers are owned by the caller and must be deleted separately
 *       before this destructor is called.
 *
 * @warning If dllPtr is nullptr (constructor failed), dlclose() behavior
 *          is undefined. Current implementation does not check this.
 */
TicketCreator::~TicketCreator() {
  // Unload the shared library and free resources
  // Note: This does not affect Ticket instances created by create()
  // Caller must delete those separately before destroying the creator
  if (dllPtr) {
    dlclose(dllPtr);
  }
}

/**
 * @brief Instantiates a Ticket model using the loaded plugin's factory function
 *
 * Uses dlsym() to retrieve the "createTicketFromDll" function pointer from the
 * loaded library, then calls it to create a concrete Ticket model instance.
 *
 * Plugin Contract (enforced by dlsym lookup):
 * - Plugin must export: extern "C" Ticket* createTicketFromDll(nlohmann::json,
 * TicketSystem&)
 * - extern "C" prevents C++ name mangling, ensuring symbol is findable
 * - Function must allocate Ticket on heap and return pointer
 * - Caller owns the returned pointer (must delete when done)
 * - Config contains ticket data (typically from API response JSON)
 * - TicketSystem reference allows model to make API calls if needed
 *
 * Model vs System Difference:
 * - System creators return interfaces (TicketSystem*, AddressSystem*, Ui*)
 * - Model creators return data structures (Ticket*)
 * - Models may hold reference to System for API interaction
 * - This allows models to be "smart" (can update themselves via API)
 *
 * Implementation Details:
 * - dlsym() searches for symbol "createTicketFromDll" in loaded library
 * - reinterpret_cast converts void* to function pointer (ticketDllCreate* type)
 * - Function is called immediately with config and api parameters
 *
 * @param config JSON object containing ticket data
 *               Typically the JSON response from ticket system API
 *               Example: Ticket system API response JSON with id, subject,
 * status, etc.
 * @param api Reference to TicketSystem for API interaction
 *            Allows ticket model to fetch additional data or update itself
 *            Example: ticket.update() internally calls api.updateTicket()
 *
 * @return Heap-allocated Ticket* (caller must delete)
 *
 * @throws std::runtime_error If "createTicketFromDll" symbol not found in
 * library Error message format: "function not found: <dlerror details>"
 *
 * @note Config is passed by value (copy), not by reference
 *       This differs from other creators which pass config by reference
 * @note Repeated calls create multiple independent Ticket instances
 */
Ticket *TicketCreator::create(nlohmann::json config, TicketSystem &api) {
  // Retrieve the factory function pointer from the loaded library
  // Symbol name "createTicketFromDll" must match the extern "C" export in
  // plugin reinterpret_cast: dlsym returns void*, we cast to function pointer
  // type
  auto creatorFunction =
      reinterpret_cast<ticketDllCreate *>(dlsym(dllPtr, "createTicketFromDll"));

  if (!creatorFunction) {
    // Symbol not found - plugin is missing the required factory function
    // Common cause: Plugin did not use extern "C" or function has wrong
    // signature
    throw std::runtime_error(std::string("function not found: ") + dlerror());
  }

  // Call the factory function to instantiate the concrete Ticket model
  // Config is passed by value (copy) - plugin receives its own copy
  // TicketSystem reference allows model to make API calls
  // Returned pointer is owned by caller (must delete when done)
  return (*creatorFunction)(config, api);
}
