#pragma once

#include <cstddef>
#include <cstdint>
#include <errno.h>
#include <system_error>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <vector>

#include "zero_copy_spsc.hpp"

namespace sf {
    template<typename T>
    struct channel {
    public:
        explicit channel(std::size_t capacity) : m_buffer(capacity) {
            if((m_event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) < 0) {
                throw std::system_error(errno, std::generic_category(), "Failed to create eventfd");
            }

            if((m_epoll_fd = epoll_create1(0)) < 0) {
                throw std::system_error(errno, std::generic_category(), "Failed to create epoll");
            }

            struct epoll_event ev;
            ev.events = EPOLLIN;
            ev.data.fd = m_event_fd;

            if(epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, m_event_fd, &ev) < 0) {
                throw std::system_error(errno, std::generic_category(), "Failed to register eventfd");
            }
        }

        ~channel() {
            if(m_event_fd >= 0) {
                close(m_event_fd);
            }

            if(m_epoll_fd >= 0) {
                close(m_epoll_fd);
            }
        }

        /*
         * Return a reference to the internal buffer.
         */
        zc_spsc<T>& buffer() noexcept {
            return m_buffer;
        }

        /*
         * Return a reference to the internal event signaler.
         */
        int fd() noexcept {
            return m_event_fd;
        }

        /*
         * Publish an item to the channel.
         *
         * @param init_func Initializer function (should be of type void (T *)).
         * 
         * @return 0 on success, -1 on failure.
         */
        template<typename Func>
        int publish(Func&& init_func) noexcept {
            // Acquire the next slot
            if(auto *slot = m_buffer.write()) {
                // Try to call the initializer function on the slot
                try {
                    init_func(slot);
                } catch(...) {
                    return -1;
                }

                // Try to commit the slot
                if(m_buffer.commit()) {
                    // Slot committed, signal to the event file that there are 
                    // new items in the buffer.
                    uint64_t signal = 1;
                    ssize_t result = write(m_event_fd, &signal, sizeof(signal));
                    return 0;
                }
            }

            return -1;
        }

        /*
         * Wait for the pipe to produce an item and then consume it.
         *
         * @param timeout Max time to wait in ms (-1 for infinity).
         * @param func Consumption function (should be of type void (T *)).
         * 
         * @return -1 on failure, 1 on interrupt, 0 on success.
         */
        template<typename Func>
        int wait(int timeout, Func&& func) noexcept {
            // Sleep until an item populates the buffer or the timeout occurs.
            int nfds = epoll_wait(m_epoll_fd, &m_event, 1, timeout);
            if(nfds < 0) {
                // Interrupt means we can retry.
                if(errno == EINTR) {
                    return 1;
                }

                // Another error should break.
                return -1;
            }

            if(m_event.data.fd == m_event_fd) {
                // Consume all the signals in the event file.
                uint64_t signals;
                ssize_t result = read(m_event_fd, &signals, sizeof(signals));

                // Consume all available slots in the buffer.
                while(auto *slot = m_buffer.read()) {
                    func(slot);
                    m_buffer.release();
                }
            }

            return 0;
        }
        
    private:
        zc_spsc<T> m_buffer;
        int m_event_fd = -1;
        int m_epoll_fd = -1;
        struct epoll_event m_event;
    };
}