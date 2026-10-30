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

template<typename T>
class ZeroCopySPSC {
    static_assert(std::is_trivially_constructible_v<T>);
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(std::is_trivially_destructible_v<T>);
    static_assert(
        std::atomic<uint64_t>::is_always_lock_free,
        "64-bit atomics must be lock-free."
    );

public:
    struct Block {
        T *data = nullptr;
        size_t size = 0;

        explicit operator bool() const noexcept {
            return data != nullptr;
        }
    };

    explicit ZeroCopySPSC(size_t capacity) : m_capacity(capacity) {
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

        m_storage = static_cast<char *>(
            ::operator new(
                m_capacity * sizeof(T),
                std::align_val_t{alignof(T)}
            )
        );

        T *objects = reinterpret_cast<T *>(m_storage);

        for(size_t i = 0; i < m_capacity; i++) {
            std::construct_at(objects + i);
        }
    }

    ~ZeroCopySPSC() {
        ::operator delete(
            m_storage,
            std::align_val_t{alignof(T)}
        );
    }

    ZeroCopySPSC(const ZeroCopySPSC&) = delete;
    ZeroCopySPSC& operator=(const ZeroCopySPSC&) = delete;

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
        return get_ptr(w);
    }

    /*
     * Increment the buffer write index to lock a data point
     * in the buffer.
     * 
     * @return True if the commit passed, false otherwise.
     */
    bool commit() noexcept {
        if(m_write_reserved != 1) {
            return false; // Incorrect commit size.
        }

        // SP so we can use relaxed from the write side.
        uint64_t w = m_write_index.load(std::memory_order_relaxed);

        // Commit the change
        m_write_reserved = 0;
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
        return get_ptr(r);
    }

    /*
     * Increment the buffer read index to free a data point
     * from the buffer.
     * 
     * @return True if the release passed, false otherwise.
     */
    bool release() noexcept {
        if(m_read_reserved != 1) {
            return false; // Incorrect release size.
        }

        // SC so we can use relaxed from the read side.
        uint64_t r = m_read_index.load(std::memory_order_relaxed);

        // Release the object
        m_read_reserved = 0;
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
    Block write_block(size_t requested) noexcept {
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

        Block block;
        block.size = std::min({
            requested,
            static_cast<size_t>(contiguous),
            static_cast<size_t>(available)
        });

        if(block.size > 0) {
            block.data = get_ptr(w);
        }

        m_write_reserved = block.size;

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
        if(block_size == 0 || m_write_reserved != block_size) {
            return 0; // Invalid commit size.
        }

        const uint64_t w =
            m_write_index.load(std::memory_order_relaxed);

        // Commit the block
        m_write_reserved = 0;
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
    Block read_block(size_t requested) noexcept {
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

        Block block;
        block.size = std::min({
            requested,
            static_cast<size_t>(contiguous),
            static_cast<size_t>(used)
        });

        if(block.size > 0) {
            block.data = get_ptr(r);
        }

        m_read_reserved = block.size;
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
        if(block_size == 0 || m_read_reserved != block_size) {
            return 0; // Invalid release size.
        }

        // SC so we can use relaxed from the read side.
        uint64_t r = m_read_index.load(std::memory_order_relaxed);

        // Release the block
        m_read_reserved = 0;
        m_read_index.store(r + block_size, std::memory_order_release);
        
        return block_size;
    }
    
private:
    const size_t m_capacity;

    // Using aligned storage prevents premature object construction.
    char *m_storage = nullptr;

    // Use cache-alignment to avoid false sharing between indexes
    alignas(std::hardware_destructive_interference_size) 
    std::atomic<uint64_t> m_write_index{0};
    
    alignas(std::hardware_destructive_interference_size) 
    std::atomic<uint64_t> m_read_index{0};

    size_t m_write_reserved = 0;
    size_t m_read_reserved = 0;

    T *get_ptr(uint64_t index) {
        const size_t i = static_cast<size_t>(
            index & (m_capacity - 1)
        );

        return reinterpret_cast<T *>(
            m_storage + i * sizeof(T)
        );
    }
};