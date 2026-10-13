#pragma once
#include <cstddef>
#include <new>
#include <type_traits>

#if defined(ARDUINO_ARCH_ESP32) && __has_include(<esp_heap_caps.h>)
#include <esp_heap_caps.h>
#define AGENT_LINK_TASK_STORAGE_USE_HEAP_CAPS 1
#else
#define AGENT_LINK_TASK_STORAGE_USE_HEAP_CAPS 0
#endif

namespace agentlink {

// Task-only fixed storage. ESP32 reserves it at setup/start, preferring PSRAM
// and falling back to internal RAM. Portable builds retain an ordinary static
// value, with no allocation. This must never contain data accessed from an ISR
// or while flash/cache operations have paused the scheduler.
//
// No method performs synchronization. Prepare before publishing to readers;
// release only after all readers have stopped. get() requires prepared().
// Long-lived SDK singletons may retain the reservation across stop/start.
template<class T> class TaskStorage {
    static_assert(std::is_nothrow_default_constructible<T>::value,
                  "TaskStorage requires a nonthrowing default constructor");
public:
    TaskStorage() = default;
    ~TaskStorage() {
#if AGENT_LINK_TASK_STORAGE_USE_HEAP_CAPS
        release();
#endif
    }
    TaskStorage(const TaskStorage&) = delete;
    TaskStorage& operator=(const TaskStorage&) = delete;

    bool prepare() {
#if AGENT_LINK_TASK_STORAGE_USE_HEAP_CAPS
        if (value_) return true;
        void* memory = heap_caps_malloc(sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!memory) memory = heap_caps_malloc(sizeof(T), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!memory) return false;
        value_ = ::new (memory) T();
#else
        prepared_ = true;
#endif
        return true;
    }
    bool prepared() const {
#if AGENT_LINK_TASK_STORAGE_USE_HEAP_CAPS
        return value_ != nullptr;
#else
        return prepared_;
#endif
    }
    T& get() {
#if AGENT_LINK_TASK_STORAGE_USE_HEAP_CAPS
        return *value_;
#else
        return value_;
#endif
    }
    const T& get() const {
#if AGENT_LINK_TASK_STORAGE_USE_HEAP_CAPS
        return *value_;
#else
        return value_;
#endif
    }
    void release() {
#if AGENT_LINK_TASK_STORAGE_USE_HEAP_CAPS
        if (!value_) return;
        value_->~T();
        heap_caps_free(value_);
        value_ = nullptr;
#else
        if (!prepared_) return;
        value_.~T();
        ::new (static_cast<void*>(&value_)) T();
        prepared_ = false;
#endif
    }
private:
#if AGENT_LINK_TASK_STORAGE_USE_HEAP_CAPS
    T* value_ = nullptr;
#else
    T value_{};
    bool prepared_ = false;
#endif
};

} // namespace agentlink
