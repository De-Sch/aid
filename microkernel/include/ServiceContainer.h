/**
 * @file ServiceContainer.h
 * @brief Dependency injection container for microkernel architecture
 *
 * ServiceContainer implements a type-safe dependency injection container that
 * manages service lifetimes and provides dependency resolution for the AID
 * microkernel system.
 *
 * Key Features:
 * - Type-safe service registration and retrieval using std::type_index
 * - Automatic type inference via templates
 * - Runtime service availability checking
 * - Safe cleanup avoiding plugin unloading issues
 *
 * Architecture Note:
 * The container intentionally does NOT delete services in the destructor to
 * avoid vtable invalidation issues when dlclose() unloads plugin shared
 * libraries. Services are owned and cleaned up by their original creator
 * classes.
 *
 * Usage Example:
 * @code
 *   ServiceContainer container;
 *   auto ticketSystem = std::make_unique<TicketSystem>();
 *   container.registerService(std::move(ticketSystem));
 *   TicketSystem& ts = container.getService<TicketSystem>();
 * @endcode
 */

#pragma once
#include <functional>
#include <memory>
#include <stdexcept>
#include <typeindex>
#include <unordered_map>

/**
 * @class ServiceContainer
 * @brief Type-safe dependency injection container for managing system services
 *
 * Provides centralized service management replacing the deprecated global
 * pointer pattern. Uses std::type_index for type-safe service lookup and
 * storage.
 */
class ServiceContainer {
public:
  /**
   * @brief Register a service in the container
   * @tparam T The service type to register
   * @param service Unique pointer to service instance (ownership transferred)
   *
   * Registers a service instance that can be retrieved later by type.
   * Takes ownership of the service but intentionally does NOT delete it
   * in the destructor to avoid plugin unloading issues.
   *
   * Note: Service ownership is released from the unique_ptr but the raw
   * pointer remains valid as long as the creator class keeps the plugin loaded.
   */
  template <typename T> void registerService(std::unique_ptr<T> service) {
    std::type_index typeIndex(typeid(T));
    void *ptr = service.release(); // Release ownership
    services[typeIndex] = ptr;
    // Store custom deleter (currently unused but reserved for future use)
    deleters[typeIndex] = [](void *p) { delete static_cast<T *>(p); };
  }

  /**
   * @brief Retrieve a registered service by type
   * @tparam T The service type to retrieve
   * @return Reference to the registered service
   * @throws std::runtime_error if service is not registered
   *
   * Type-safe service lookup using RTTI. The service must have been
   * previously registered or this will throw an exception.
   */
  template <typename T> T &getService() {
    std::type_index typeIndex(typeid(T));
    auto it = services.find(typeIndex);
    if (it == services.end()) {
      throw std::runtime_error("Service not registered: " +
                               std::string(typeid(T).name()));
    }
    return *static_cast<T *>(it->second);
  }

  /**
   * @brief Check if a service is registered
   * @tparam T The service type to check
   * @return true if service is registered, false otherwise
   *
   * Non-throwing way to check service availability before attempting retrieval.
   */
  template <typename T> bool hasService() const {
    std::type_index typeIndex(typeid(T));
    return services.find(typeIndex) != services.end();
  }

  /**
   * @brief Destructor - Intentionally does NOT delete services
   *
   * Services are NOT deleted to avoid vtable invalidation when dlclose()
   * unloads plugin shared libraries. Creator classes are responsible for
   * managing service lifetimes and ensuring proper cleanup order.
   */
  ~ServiceContainer() {
    // Don't delete services - they'll be cleaned up by their original creators
    // This avoids the plugin unloading issue where dlclose() invalidates
    // vtables
    services.clear();
    deleters.clear();
  }

private:
  /**
   * @brief Map of type indices to service pointers
   *
   * Uses std::type_index as key for type-safe service lookup.
   * Values are void* to allow storage of any service type.
   */
  std::unordered_map<std::type_index, void *> services;

  /**
   * @brief Map of type indices to custom deleter functions
   *
   * Stores deleter functions for each service type. Currently unused
   * due to plugin unloading concerns, but reserved for future use if
   * a safe deletion strategy is implemented.
   */
  std::unordered_map<std::type_index, std::function<void(void *)>> deleters;
};