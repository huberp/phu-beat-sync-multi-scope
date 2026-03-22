#pragma once

#include "Event.h"
#include <algorithm>
#include <vector>

namespace phu {
namespace events {

/**
 * Base EventSource template
 *
 * Provides common listener management functionality for all event sources.
 *
 * @tparam ListenerType The listener interface type (e.g., GlobalsEventListener)
 */
template <typename ListenerType> class EventSource {
  protected:
    std::vector<ListenerType*> listeners;

  public:
    virtual ~EventSource() = default;

    ListenerType* addEventListener(ListenerType* listener) {
        if (listener &&
            std::find(listeners.begin(), listeners.end(), listener) == listeners.end()) {
            listeners.push_back(listener);
        }
        return listener;
    }

    bool removeEventListener(ListenerType* listener) {
        auto it = std::find(listeners.begin(), listeners.end(), listener);
        if (it != listeners.end()) {
            listeners.erase(it);
            return true;
        }
        return false;
    }

    size_t getListenerCount() const {
        return listeners.size();
    }
};

} // namespace events
} // namespace phu
