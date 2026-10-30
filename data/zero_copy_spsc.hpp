#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>

#include "block.hpp"

namespace sf {
    template<typename T>
    class zc_spsc {
        static_assert(std::is_trivially_constructible_v<T>);
        static_assert(std::is_trivially_copyable_v<T>);
        static_assert(std::is_trivially_destructible_v<T>);
        static_assert(
            std::atomic<uint64_t>::is_always_lock_free,
            "64-bit atomics must be lock-free."
        );

    public:
        explicit zc_spsc(size_t capacity) : m_capacity(capacity) {
            if(
                capacity == 0 ||
                (capacity & (capacity - 1)) != 0
            ) {
                throw std::invalid_argument(
                    "Capacity must be a nonzero power of two."
                );
            }

            if(capacity > std::numeric_limits<size_t>::max() / sizeof(T)) {
                throw std::bad_array_new_length();
            }

            m_storage = std::make_unique<T[]>(capacity);
        }

        zc_spsc(const zc_spsc&) = delete;
        zc_spsc& operator=(const zc_spsc&) = delete;

        /*
        * Get access to the next buffer slot.
        * **commit should ALWAYS be called after this to
        * ensure the slot is published**
        *
        * @return Pointer reference to a single buffer slot.
        */
        T *write() noexcept {
            if(m_write_reserved != 0) {
                return nullptr; // Outstanding write transaction.
            }

            // SP so we can use relaxed from write side.
            uint64_t w = m_write_index.load(std::memory_order_relaxed);
            uint64_t r = m_read_index.load(std::memory_order_acquire);

            if((w - r) == m_capacity) {
                return nullptr; // Buffer is full.
            }

            m_write_reserved = 1;
            m_write_reservation = reservation_t::SINGLE;
            return get_ptr(w);
        }

        /*
        * Increment the buffer write index to lock a data point
        * in the buffer.
        * 
        * @return True if the commit passed, false otherwise.
        */
        bool commit() noexcept {
            if(m_write_reserved != 1 || m_write_reservation != reservation_t::SINGLE) {
                return false; // Incorrect commit size.
            }

            // SP so we can use relaxed from the write side.
            uint64_t w = m_write_index.load(std::memory_order_relaxed);

            // Commit the change
            m_write_reserved = 0;
            m_write_reservation = reservation_t::NONE;
            m_write_index.store(w + 1, std::memory_order_release);

            return true;
        }

        /*
        * Get access to the next read slot.
        * **release should ALWAYS be called after this to
        * ensure the slot is freed**
        * 
        * @return Pointer reference to a single buffer slot.
        */
        T *read() noexcept {
            if(m_read_reserved != 0) {
                return nullptr; // Outstanding read transaction.
            }

            // SC so we can use relaxed from read side.
            uint64_t r = m_read_index.load(std::memory_order_relaxed);
            uint64_t w = m_write_index.load(std::memory_order_acquire);

            if(r == w) {
                return nullptr; // Buffer is empty
            }

            m_read_reserved = 1;
            m_read_reservation = reservation_t::SINGLE;
            return get_ptr(r);
        }

        /*
        * Increment the buffer read index to free a data point
        * from the buffer.
        * 
        * @return True if the release passed, false otherwise.
        */
        bool release() noexcept {
            if(m_read_reserved != 1 || m_read_reservation != reservation_t::SINGLE) {
                return false; // Incorrect release size.
            }

            // SC so we can use relaxed from the read side.
            uint64_t r = m_read_index.load(std::memory_order_relaxed);

            // Release the object
            m_read_reserved = 0;
            m_read_reservation = reservation_t::NONE;
            m_read_index.store(r + 1, std::memory_order_release);

            return true;
        }

        /*
        * Get access to a block of buffer write slots.
        * **commit_block should ALWAYS be called after this to 
        * ensure the slot is published**
        *
        * @param requested Request this many slots from the buffer.
        * 
        * @return The write block.
        */
        block<T> write_block(size_t requested) noexcept {
            if(m_write_reserved != 0) {
                return {}; // Outstanding write transaction.
            }

            // SP so we can use relaxed from write side.
            uint64_t w = m_write_index.load(std::memory_order_relaxed);
            uint64_t r = m_read_index.load(std::memory_order_acquire);

            const uint64_t index = 
                w & (m_capacity - 1);

            // The block returns a pointer to contigous memory,
            // so we need to check if the requested block will wrap.
            const uint64_t contiguous = 
                m_capacity - index;

            const uint64_t used = w - r;
            const uint64_t available = m_capacity - used;

            block<T> block;
            block.size = std::min({
                requested,
                static_cast<size_t>(contiguous),
                static_cast<size_t>(available)
            });

            if(block.size > 0) {
                block.data = get_ptr(w);
            }

            m_write_reserved = block.size;
            m_write_reservation = reservation_t::BLOCK;

            // Return the block to write to.
            return block;
        }

        /*
        * Advance the buffer write index by block size to lock
        * a block of data points in the buffer.
        * 
        * @param block_size Size of the block to publish (should be *actual from write_block).
        * 
        * @return The number of published data points (success = block_size, failure < block_size).
        */
        size_t commit_block(size_t block_size) noexcept {
            if(block_size == 0 || m_write_reserved != block_size || m_write_reservation != reservation_t::BLOCK) {
                return 0; // Invalid commit size.
            }

            const uint64_t w =
                m_write_index.load(std::memory_order_relaxed);

            // Commit the block
            m_write_reserved = 0;
            m_write_reservation = reservation_t::NONE;
            m_write_index.store(w + block_size, std::memory_order_release);
            
            return block_size;
        }

        /*
        * Get access to a block of buffer read slots.
        * **release_block should ALWAYS be called after this to
        * ensure the slot is freed**
        * 
        * @param requested Request this many slots from the buffer.
        * 
        * @return Pointer reference to the start of the block.
        */
        block<T> read_block(size_t requested) noexcept {
            if(m_read_reserved != 0) {
                return {}; // Outstanding read transaction.
            }

            // SC so we can use relaxed from read side.
            uint64_t r = m_read_index.load(std::memory_order_relaxed);
            uint64_t w = m_write_index.load(std::memory_order_acquire);

            const uint64_t index = 
                r & (m_capacity - 1);

            // The block returns a pointer to contigous memory,
            // so we need to check if the requested block will wrap.
            const uint64_t contiguous = 
                m_capacity - index;

            const uint64_t used = w - r;

            block<T> block;
            block.size = std::min({
                requested,
                static_cast<size_t>(contiguous),
                static_cast<size_t>(used)
            });

            if(block.size > 0) {
                block.data = get_ptr(r);
            }

            m_read_reserved = block.size;
            m_read_reservation = reservation_t::BLOCK;
            return block;
        }

        /*
        * Advance the buffer read index by the block size to free
        * a block of data points in the buffer.
        * 
        * @param block_size Size of the block to release (should be *actual from read_block).
        * 
        * @return The number of published data points (success = block_size, failure < block_size).
        */
        size_t release_block(size_t block_size) noexcept {
            if(block_size == 0 || m_read_reserved != block_size || m_read_reservation != reservation_t::BLOCK) {
                return 0; // Invalid release size.
            }

            // SC so we can use relaxed from the read side.
            uint64_t r = m_read_index.load(std::memory_order_relaxed);

            // Release the block
            m_read_reserved = 0;
            m_read_reservation = reservation_t::NONE;
            m_read_index.store(r + block_size, std::memory_order_release);
            
            return block_size;
        }
        
    private:
        const size_t m_capacity;
        std::unique_ptr<T[]> m_storage = nullptr;

        // Use cache-alignment to avoid false sharing between indexes
        alignas(64) std::atomic<uint64_t> m_write_index{0};
        
        alignas(64) std::atomic<uint64_t> m_read_index{0};

        enum class reservation_t {
            NONE,
            SINGLE,
            BLOCK
        };

        reservation_t m_write_reservation = reservation_t::NONE;
        reservation_t m_read_reservation = reservation_t::NONE;

        size_t m_write_reserved = 0;
        size_t m_read_reserved = 0;

        T *get_ptr(uint64_t index) {
            const size_t i = static_cast<size_t>(
                index & (m_capacity - 1)
            );

            return m_storage.get() + i;
        }
    };
}