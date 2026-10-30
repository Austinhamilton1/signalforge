#include <catch2/catch_test_macros.hpp>

#include "zero_copy_spsc.hpp"

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <thread>

using namespace sf;

TEST_CASE("SPSC validates capacity", "[spsc] [setup]") {
    REQUIRE_THROWS_AS(zc_spsc<float>(0), std::invalid_argument);
    REQUIRE_THROWS_AS(zc_spsc<float>(3), std::invalid_argument);
    REQUIRE_THROWS_AS(zc_spsc<float>(7), std::invalid_argument);

    REQUIRE_NOTHROW(zc_spsc<float>(1));
    REQUIRE_NOTHROW(zc_spsc<float>(8));
}

TEST_CASE("SPSC reads and writes a single element", "[spsc] [sanity]") {
    zc_spsc<float> buffer(8);

    float *write_ptr = buffer.write();

    REQUIRE(write_ptr != nullptr);

    *write_ptr = 42.5f;

    REQUIRE(buffer.commit());

    float *read_ptr = buffer.read();

    REQUIRE(read_ptr != nullptr);
    REQUIRE(*read_ptr == 42.5f);

    REQUIRE(buffer.release());

    REQUIRE(buffer.read() == nullptr);
}

TEST_CASE("SPSC returns nullptr when empty", "[spsc] [sanity]") {
    zc_spsc<int> buffer(8);

    REQUIRE(buffer.read() == nullptr);
}

TEST_CASE("SPSC returns nullptr when full", "[spsc] [sanity]") {
    zc_spsc<int> buffer(4);

    for(int i = 0; i < 4; ++i) {
        int *ptr = buffer.write();

        REQUIRE(ptr != nullptr);

        *ptr = i;

        REQUIRE(buffer.commit());
    }

    REQUIRE(buffer.write() == nullptr);
}

TEST_CASE("SPSC preserves FIFO ordering", "[spsc] [fifo]") {
    zc_spsc<int> buffer(8);

    for(int i = 0; i < 8; ++i) {
        int *ptr = buffer.write();

        REQUIRE(ptr != nullptr);

        *ptr = i;

        REQUIRE(buffer.commit());
    }

    for(int i = 0; i < 8; ++i) {
        int *ptr = buffer.read();

        REQUIRE(ptr != nullptr);
        REQUIRE(*ptr == i);

        REQUIRE(buffer.release());
    }
}

TEST_CASE("SPSC supports wraparound", "[spsc] [fifo]") {
    zc_spsc<int> buffer(4);

    // Fill the buffer.
    for(int i = 0; i < 4; ++i) {
        int *ptr = buffer.write();

        REQUIRE(ptr != nullptr);

        *ptr = i;

        REQUIRE(buffer.commit());
    }

    // Consume everything.
    for(int i = 0; i < 4; ++i) {
        int *ptr = buffer.read();

        REQUIRE(ptr != nullptr);
        REQUIRE(*ptr == i);

        REQUIRE(buffer.release());
    }

    // Write again after wrapping.
    for(int i = 4; i < 8; ++i) {
        int *ptr = buffer.write();

        REQUIRE(ptr != nullptr);

        *ptr = i;

        REQUIRE(buffer.commit());
    }

    // Verify the second sequence.
    for(int i = 4; i < 8; ++i) {
        int *ptr = buffer.read();

        REQUIRE(ptr != nullptr);
        REQUIRE(*ptr == i);

        REQUIRE(buffer.release());
    }
}

TEST_CASE("SPSC prevents multiple write reservations", "[spsc] [reservation]") {
    zc_spsc<int> buffer(8);

    int *ptr = buffer.write();

    REQUIRE(ptr != nullptr);

    // A second reservation must fail.
    REQUIRE(buffer.write() == nullptr);

    REQUIRE(buffer.write_block(4).size == 0);

    *ptr = 123;

    REQUIRE(buffer.commit());

    // Reservation is now released.
    REQUIRE(buffer.write() != nullptr);
}

TEST_CASE("SPSC prevents multiple read reservations", "[spsc] [reservation]") {
    zc_spsc<int> buffer(8);

    int *write_ptr = buffer.write();

    REQUIRE(write_ptr != nullptr);

    *write_ptr = 123;

    REQUIRE(buffer.commit());

    int *read_ptr = buffer.read();

    REQUIRE(read_ptr != nullptr);

    REQUIRE(buffer.read() == nullptr);
    REQUIRE(buffer.read_block(4).size == 0);

    REQUIRE(*read_ptr == 123);

    REQUIRE(buffer.release());

    // Reservation is now released.
    REQUIRE(buffer.read() == nullptr);
}

TEST_CASE("SPSC rejects invalid commits and releases", "[spsc] [reservation]") {
    zc_spsc<int> buffer(8);

    REQUIRE_FALSE(buffer.commit());
    REQUIRE_FALSE(buffer.release());

    REQUIRE(buffer.commit_block(1) == 0);
    REQUIRE(buffer.release_block(1) == 0);
}

TEST_CASE("SPSC prevents single write reservation during single reservation", "[spsc] [reservation]") {
    zc_spsc<int> buffer(8);

    int *ptr = buffer.write();
    REQUIRE(ptr != nullptr);

    REQUIRE(buffer.write() == nullptr);
    REQUIRE(buffer.write_block(4).size == 0);

    *ptr = 42;
    REQUIRE(buffer.commit());
}

TEST_CASE("SPSC prevents block write reservation during block reservation", "[spsc] [reservation]") {
    zc_spsc<int> buffer(8);

    auto block = buffer.write_block(4);
    REQUIRE(block.size == 4);

    REQUIRE(buffer.write() == nullptr);
    REQUIRE(buffer.write_block(2).size == 0);

    REQUIRE(buffer.commit_block(4) == 4);
}

TEST_CASE("SPSC prevents block write reservation during single reservation", "[spsc] [reservation]") {
    zc_spsc<int> buffer(8);

    int *ptr = buffer.write();
    REQUIRE(ptr != nullptr);

    *ptr = 42;

    REQUIRE(buffer.write_block(4).size == 0);

    REQUIRE(buffer.commit());
}

TEST_CASE("SPSC prevents single write reservation during block reservation", "[spsc] [reservation]") {
    zc_spsc<int> buffer(8);

    auto block = buffer.write_block(4);
    REQUIRE(block.size == 4);

    REQUIRE(buffer.write() == nullptr);

    REQUIRE(buffer.commit_block(4) == 4);
}

TEST_CASE("SPSC prevents single read reservation during single reservation", "[spsc] [reservation]") {
    zc_spsc<int> buffer(8);

    int *write_ptr = buffer.write();
    REQUIRE(write_ptr != nullptr);

    *write_ptr = 42;
    REQUIRE(buffer.commit());

    int *read_ptr = buffer.read();
    REQUIRE(read_ptr != nullptr);

    REQUIRE(buffer.read() == nullptr);
    REQUIRE(buffer.read_block(4).size == 0);

    REQUIRE(buffer.release());
}

TEST_CASE("SPSC prevents block read reservation during block reservation", "[spsc] [reservation]") {
    zc_spsc<int> buffer(8);

    auto write_block = buffer.write_block(4);
    REQUIRE(write_block.size == 4);

    for(size_t i = 0; i < 4; ++i) {
        write_block.data[i] = static_cast<int>(i);
    }

    REQUIRE(buffer.commit_block(4) == 4);

    auto read_block = buffer.read_block(4);
    REQUIRE(read_block.size == 4);

    REQUIRE(buffer.read() == nullptr);
    REQUIRE(buffer.read_block(2).size == 0);

    REQUIRE(buffer.release_block(4) == 4);
}

TEST_CASE("SPSC prevents block read reservation during single reservation", "[spsc] [reservation]") {
    zc_spsc<int> buffer(8);

    int *write_ptr = buffer.write();
    REQUIRE(write_ptr != nullptr);

    *write_ptr = 42;
    REQUIRE(buffer.commit());

    int *read_ptr = buffer.read();
    REQUIRE(read_ptr != nullptr);

    REQUIRE(buffer.read_block(4).size == 0);

    REQUIRE(buffer.release());
}

TEST_CASE("SPSC prevents single read reservation during block reservation", "[spsc] [reservation]") {
    zc_spsc<int> buffer(8);

    auto write_block = buffer.write_block(4);
    REQUIRE(write_block.size == 4);

    for(size_t i = 0; i < 4; ++i) {
        write_block.data[i] = static_cast<int>(i);
    }

    REQUIRE(buffer.commit_block(4) == 4);

    auto read_block = buffer.read_block(4);
    REQUIRE(read_block.size == 4);

    REQUIRE(buffer.read() == nullptr);

    REQUIRE(buffer.release_block(4) == 4);
}

TEST_CASE("SPSC allows switching reservation types after commit and release", "[spsc] [reservation]") {
    zc_spsc<int> buffer(8);

    // Single write.
    int *single_write = buffer.write();
    REQUIRE(single_write != nullptr);

    *single_write = 10;
    REQUIRE(buffer.commit());

    // Block read.
    auto block_read = buffer.read_block(1);
    REQUIRE(block_read.size == 1);
    REQUIRE(block_read.data[0] == 10);

    REQUIRE(buffer.release_block(1) == 1);

    // Block write.
    auto block_write = buffer.write_block(2);
    REQUIRE(block_write.size == 2);

    block_write.data[0] = 20;
    block_write.data[1] = 30;

    REQUIRE(buffer.commit_block(2) == 2);

    // Single read.
    int *single_read = buffer.read();
    REQUIRE(single_read != nullptr);
    REQUIRE(*single_read == 20);

    REQUIRE(buffer.release());

    single_read = buffer.read();
    REQUIRE(single_read != nullptr);
    REQUIRE(*single_read == 30);

    REQUIRE(buffer.release());
}

TEST_CASE("SPSC writes and reads a block", "[spsc] [block]") {
    zc_spsc<int> buffer(8);

    auto write_block = buffer.write_block(8);

    REQUIRE(write_block);
    REQUIRE(write_block.size == 8);

    for(size_t i = 0; i < write_block.size; ++i) {
        write_block.data[i] = static_cast<int>(i * 10);
    }

    REQUIRE(buffer.commit_block(write_block.size) == 8);

    auto read_block = buffer.read_block(8);

    REQUIRE(read_block);
    REQUIRE(read_block.size == 8);

    for(size_t i = 0; i < read_block.size; ++i) {
        REQUIRE(read_block.data[i] == static_cast<int>(i * 10));
    }

    REQUIRE(buffer.release_block(read_block.size) == 8);
}

TEST_CASE("SPSC limits blocks to contiguous memory", "[spsc] [block]") {
    zc_spsc<int> buffer(8);

    // Move both indices to position 6.
    for(int i = 0; i < 6; ++i) {
        int *ptr = buffer.write();

        REQUIRE(ptr != nullptr);

        *ptr = i;

        REQUIRE(buffer.commit());
    }

    for(int i = 0; i < 6; ++i) {
        int *ptr = buffer.read();

        REQUIRE(ptr != nullptr);
        REQUIRE(*ptr == i);

        REQUIRE(buffer.release());
    }

    // Only two contiguous slots remain before the end.
    auto first = buffer.write_block(8);

    REQUIRE(first);
    REQUIRE(first.size == 2);

    first.data[0] = 100;
    first.data[1] = 101;

    REQUIRE(buffer.commit_block(first.size) == 2);

    // The next block starts at index zero.
    auto second = buffer.write_block(8);

    REQUIRE(second);
    REQUIRE(second.size == 6);

    for(size_t i = 0; i < second.size; ++i) {
        second.data[i] = static_cast<int>(200 + i);
    }

    REQUIRE(buffer.commit_block(second.size) == 6);

    auto read_first = buffer.read_block(8);

    REQUIRE(read_first.size == 2);
    REQUIRE(read_first.data[0] == 100);
    REQUIRE(read_first.data[1] == 101);

    REQUIRE(buffer.release_block(read_first.size) == 2);

    auto read_second = buffer.read_block(8);

    REQUIRE(read_second.size == 6);

    for(size_t i = 0; i < 6; ++i) {
        REQUIRE(read_second.data[i] == static_cast<int>(200 + i));
    }

    REQUIRE(buffer.release_block(read_second.size) == 6);
}

TEST_CASE("SPSC rejects incorrect block commit sizes", "[spsc] [block]") {
    zc_spsc<int> buffer(8);

    auto block = buffer.write_block(4);

    REQUIRE(block.size == 4);

    // Cannot commit a different number of elements.
    REQUIRE(buffer.commit_block(3) == 0);

    // The original reservation is still active.
    REQUIRE(buffer.write() == nullptr);

    REQUIRE(buffer.commit_block(4) == 4);
}

TEST_CASE("SPSC rejects incorrect block release sizes", "[spsc] [block]") {
    zc_spsc<int> buffer(8);

    auto write_block = buffer.write_block(4);

    REQUIRE(write_block.size == 4);

    for(size_t i = 0; i < 4; ++i) {
        write_block.data[i] = static_cast<int>(i);
    }

    REQUIRE(buffer.commit_block(4) == 4);

    auto read_block = buffer.read_block(4);

    REQUIRE(read_block.size == 4);

    REQUIRE(buffer.release_block(3) == 0);

    // The original reservation is still active.
    REQUIRE(buffer.read() == nullptr);

    REQUIRE(buffer.release_block(4) == 4);
}

TEST_CASE("SPSC supports concurrent producer and consumer", "[spsc] [parallel]") {
    constexpr uint64_t iterations = 500'000;

    zc_spsc<uint64_t> buffer(64);

    std::atomic<bool> failed{false};

    std::thread producer([&]() {
        for(uint64_t i = 0; i < iterations; ++i) {
            uint64_t *ptr = nullptr;

            while((ptr = buffer.write()) == nullptr) {
                std::this_thread::yield();
            }

            *ptr = i;

            if(!buffer.commit()) {
                failed.store(true, std::memory_order_relaxed);
                return;
            }
        }
    });

    std::thread consumer([&]() {
        for(uint64_t expected = 0; expected < iterations; ++expected) {
            uint64_t *ptr = nullptr;

            while((ptr = buffer.read()) == nullptr) {
                std::this_thread::yield();
            }

            if(*ptr != expected) {
                failed.store(true, std::memory_order_relaxed);
            }

            if(!buffer.release()) {
                failed.store(true, std::memory_order_relaxed);
                return;
            }
        }
    });

    producer.join();
    consumer.join();

    REQUIRE_FALSE(failed.load());
}