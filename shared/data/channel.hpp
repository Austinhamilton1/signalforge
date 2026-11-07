#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
         * @param value Value to publish to the buffer.
         * 
         * @return 0 on success, -1 on failure.
         */
        int publish(const T& value) noexcept {
            // Acquire the next slot
            if(auto *slot = m_buffer.write()) {
                //Set the slot
                *slot = value;

                // Try to commit the slot
                if(m_buffer.commit()) {
                    m_counter.fetch_add(1, std::memory_order_relaxed);

                    // Slot committed, signal to the event file that there are 
                    // new items in the buffer.
                    uint64_t signal = 1;
                    ssize_t result = write(m_event_fd, &signal, sizeof(signal));
                    if(result < 0) {
                        return -1;
                    }

                    return 0;
                }
            }

            return -1;
        }

        /*
         * Publish n items to the channel.
         *
         * @param *values Values to publish to the buffer.
         * 
         * @return 0 on success, -1 on failure.
         */
        int publish_n(const T *values, std::size_t n) {
            if(values == nullptr || n == 0) return -1;

            std::size_t accumulator = 0;
            while(accumulator < n) {
                block<T> slots = m_buffer.write_block(n - accumulator);

                if(!slots) {
                    return -1;
                }

                std::memcpy(slots.data, values + accumulator, slots.size * sizeof(T));

                const std::size_t committed =
                    m_buffer.commit_block(slots.size);

                if(committed != slots.size) {
                    return -1;
                }

                accumulator += committed;
                m_counter.fetch_add(committed, std::memory_order_relaxed);
            }

            // Block committed, signal to the event file that there are
            // new items in the buffer.
            uint64_t signal = 1;
            ssize_t result = write(m_event_fd, &signal, sizeof(signal));
            if(result != sizeof(signal)) {
                return -1;
            }

            return 0;
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

            if(nfds == 0) {
                return 1; // Timeout
            }

            if(m_event.data.fd == m_event_fd) {
                // Consume all the signals in the event file.
                uint64_t signals;
                ssize_t result = read(m_event_fd, &signals, sizeof(signals));

                // Consume all available slots in the buffer.
                while(auto *slot = m_buffer.read()) {
                    func(slot);
                    m_buffer.release();
                    m_counter.fetch_sub(1, std::memory_order_relaxed);
                }
            }

            return 0;
        }

        /*
         * Wait for the pipe to produce n items and consume them.
         *
         * @param *output Pass in this pointer to the consumption function.
         * @param n Number of items to wait for.
         * @param timeout Max time to wait in ms (-1 for infinity).
         * @param func Consumption function (should be of type void (T *)).
         * 
         * @return -1 on failure, 1 on interrupt, 0 on success.
         */
        template<typename Func>
        int wait_n(T *output, std::size_t n, int timeout, Func&& func) noexcept {
            if(output == nullptr) return -1;

            while(m_counter.load(std::memory_order_relaxed) < n) {
                int nfds = epoll_wait(m_epoll_fd, &m_event, 1, timeout);
                if(nfds < 0) {
                    // Interrupt means we can retry.
                    if(errno == EINTR) {
                        return 1;
                    }

                    // Another error should break.
                    return -1;
                }

                if(nfds == 0) {
                    return 1; // Timeout
                }

                if(m_event.data.fd == m_event_fd) {
                    // Consume all signals in the event file.
                    uint64_t signals;
                    ssize_t result = read(m_event_fd, &signals, sizeof(signals));

                    if(result < 0 && errno != EAGAIN) {
                        return -1;
                    }
                }
            }

            // Consume a batch of n items.
            for(size_t i = 0; i < n; i++) {
                // Copy data to the output
                auto *slot = m_buffer.read();
                if(!slot) return -1;

                output[i] = *slot;

                m_buffer.release();
                m_counter.fetch_sub(1, std::memory_order_relaxed);
            }
            
            func(output);

            return 0;
        }
        
    private:
        zc_spsc<T> m_buffer;
        int m_event_fd = -1;
        int m_epoll_fd = -1;
        struct epoll_event m_event;
        std::atomic<std::size_t> m_counter{0};
    };
}