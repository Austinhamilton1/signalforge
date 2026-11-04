#pragma once

#include <stdexcept>
#include <sys/eventfd.h>
#include <unistd.h>

#include "zero_copy_spsc.hpp"

namespace sf {
    template<typename T>
    struct channel {
    public:
        explicit channel(size_t capacity) : m_buffer(capacity) {
            if((m_event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) < 0) {
                throw std::runtime_error("Could not open event fd");
            }
        }

        ~channel() {
            if(m_event_fd >= 0) {
                close(m_event_fd);
            }
        }

        zc_spsc<T>& buffer() noexcept {
            return m_buffer;
        }

        int fd() noexcept {
            return m_event_fd;
        }
        
    private:
        zc_spsc<T> m_buffer;
        int m_event_fd = -1;
    };
}