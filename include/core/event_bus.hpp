#pragma once

#include <functional>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace hft::core {

/**
 * @brief Type-erased synchronous event bus for action events
 *
 * EventBus provides a synchronous publish-subscribe mechanism with:
 * - Type-safe event registration via templates
 * - Type-erased storage (no allocation on publish)
 * - Synchronous execution (no queues, no threads)
 * - Multiple subscribers per event type
 *
 * Usage:
 *   EventBus bus;
 *   bus.subscribe<MyEvent>([](const MyEvent& e) { ... });
 *   bus.publish(MyEvent{...});
 */
class EventBus {
public:
    /**
     * @brief Subscribe to events of type Event
     * @tparam Event The event type to subscribe to
     * @param handler Callback invoked when event is published (synchronous)
     */
    template <typename Event>
    void subscribe(std::function<void(const Event&)> handler) {
        auto type = std::type_index(typeid(Event));

        // Wrap the typed handler in a type-erased wrapper
        ErasedHandler erased;
        erased.invoke = [handler = std::move(handler)](const void* event) {
            handler(*static_cast<const Event*>(event));
        };

        handlers_[type].push_back(std::move(erased));
    }

    /**
     * @brief Publish an event to all subscribers (synchronous)
     * @tparam Event The event type to publish
     * @param event The event instance to publish
     *
     * All handlers for this event type are invoked synchronously in registration order.
     * No allocation occurs during publish (handlers pre-allocated during subscribe).
     */
    template <typename Event>
    void publish(const Event& event) {
        auto type = std::type_index(typeid(Event));

        auto it = handlers_.find(type);
        if (it == handlers_.end()) {
            return; // No subscribers for this event type
        }

        // Invoke all handlers synchronously
        for (auto& handler : it->second) {
            handler.invoke(&event);
        }
    }

    /**
     * @brief Get the number of subscribers for a specific event type
     * @tparam Event The event type to query
     * @return Number of registered handlers for this event type
     */
    template <typename Event>
    size_t subscriber_count() const {
        auto type = std::type_index(typeid(Event));
        auto it = handlers_.find(type);
        return (it != handlers_.end()) ? it->second.size() : 0;
    }

    /**
     * @brief Clear all subscribers (useful for testing)
     */
    void clear() { handlers_.clear(); }

private:
    /**
     * @brief Type-erased handler wrapper
     *
     * Stores a function pointer that knows how to cast void* back to the original
     * event type and invoke the typed handler.
     */
    struct ErasedHandler {
        std::function<void(const void*)> invoke;
    };

    // Map from event type -> list of handlers
    std::unordered_map<std::type_index, std::vector<ErasedHandler>> handlers_;
};

} // namespace hft::core
