#pragma once

#include "events.hpp"

#include <array>
#include <functional>
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
        auto idx = static_cast<size_t>(Event::type_id);

        // Wrap typed handler in type-erased lambda
        auto erased = [handler = std::move(handler)](const void* event) { handler(*static_cast<const Event*>(event)); };

        handlers_[idx].push_back(std::move(erased));
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
        auto idx = static_cast<size_t>(Event::type_id);

        // Invoke all handlers synchronously
        for (auto& handler : handlers_[idx]) {
            handler(&event);
        }
    }

    /**
     * @brief Get the number of subscribers for a specific event type
     * @tparam Event The event type to query
     * @return Number of registered handlers for this event type
     */
    template <typename Event>
    size_t subscriber_count() const {
        auto idx = static_cast<size_t>(Event::type_id);
        return handlers_[idx].size();
    }

    /**
     * @brief Clear all subscribers (useful for testing)
     */
    void clear() {
        for (auto& vec : handlers_) {
            vec.clear();
        }
    }

private:
    // Array-indexed handlers for O(1) constant-time lookup
    // Replaces hash table with direct array access using Event::type_id
    std::array<std::vector<std::function<void(const void*)>>, static_cast<size_t>(EventType::COUNT)> handlers_;
};

} // namespace hft::core
