#include <array>
#include <chrono>
#include <cstdint>
#include <stdexcept>

#include <catch2/catch_test_macros.hpp>

#include "jitter.hpp"

using namespace std::chrono_literals;

namespace {
    using namespace sf;

    using TestBuffer = jitter_buf<4, 8>;
    using Packet = TestBuffer::packet_t;
    using InsertResult = TestBuffer::insert_result_t;
    using PlayoutResult = TestBuffer::playout_result_t;
    using Clock = TestBuffer::clock_t;
    using TimePoint = TestBuffer::time_point_t;

    Packet make_packet(std::uint32_t sequence, float value) {
        Packet packet{};
        packet.seq = sequence;

        for (auto& sample : packet.samples) {
            sample = value;
        }

        return packet;
    }

    bool output_equals(const std::array<float, 4>& output, float value) {
        for (const auto sample : output) {
            if (sample != value) {
                return false;
            }
        }

        return true;
    }
}

// -----------------------------------------------------------------------------
// Construction
// -----------------------------------------------------------------------------

TEST_CASE("Jitter buffer accepts valid configuration", "[jitter] [construction]") {
    REQUIRE_NOTHROW(TestBuffer{800, 4});
    REQUIRE_NOTHROW(TestBuffer{800, 1});
    REQUIRE_NOTHROW(TestBuffer{800, 8});
}

TEST_CASE("Jitter buffer rejects a zero sample rate", "[jitter] [construction]") {
    REQUIRE_THROWS_AS(TestBuffer(0, 4), std::invalid_argument);
}

TEST_CASE("Jitter buffer rejects a zero startup threshold", "[jitter] [construction]") {
    REQUIRE_THROWS_AS(TestBuffer(800, 0), std::invalid_argument);
}

TEST_CASE("Jitter buffer rejects a startup threshold larger than capacity", "[jitter] [construction]") {
    REQUIRE_THROWS_AS(TestBuffer(800, 9), std::invalid_argument);
}

// -----------------------------------------------------------------------------
// Startup
// -----------------------------------------------------------------------------

TEST_CASE("Jitter buffer starts in buffering state", "[jitter] [startup]") {
    TestBuffer buffer{800, 4};

    REQUIRE_FALSE(buffer.playing());
    REQUIRE(buffer.buffered_packets() == 0);
    REQUIRE_FALSE(buffer.next_deadline().has_value());
}

TEST_CASE("Jitter buffer waits until startup threshold is reached", "[jitter] [startup]") {
    TestBuffer buffer{800, 4};

    std::array<float, 4> output{};
    const auto start = TimePoint{};

    REQUIRE(buffer.insert(make_packet(10, 1.0f)) == InsertResult::accepted);
    REQUIRE(buffer.insert(make_packet(11, 2.0f)) == InsertResult::accepted);
    REQUIRE(buffer.insert(make_packet(12, 3.0f)) == InsertResult::accepted);

    REQUIRE(buffer.playout(start, output) == PlayoutResult::buffering);
    REQUIRE_FALSE(buffer.playing());
    REQUIRE_FALSE(buffer.next_deadline().has_value());

    REQUIRE(buffer.insert(make_packet(13, 4.0f)) == InsertResult::accepted);

    REQUIRE(buffer.playout(start, output) == PlayoutResult::audio);
    REQUIRE(buffer.playing());
    REQUIRE(output_equals(output, 1.0f));
}

TEST_CASE("Jitter buffer requires consecutive packets before startup", "[jitter] [startup]") {
    TestBuffer buffer{800, 3};

    std::array<float, 4> output{};
    const auto start = TimePoint{};

    REQUIRE(buffer.insert(make_packet(10, 1.0f)) == InsertResult::accepted);
    REQUIRE(buffer.insert(make_packet(12, 3.0f)) == InsertResult::accepted);
    REQUIRE(buffer.insert(make_packet(13, 4.0f)) == InsertResult::accepted);

    REQUIRE(buffer.buffered_packets() == 3);
    REQUIRE(buffer.playout(start, output) == PlayoutResult::buffering);
    REQUIRE_FALSE(buffer.playing());

    REQUIRE(buffer.insert(make_packet(11, 2.0f)) == InsertResult::accepted);

    REQUIRE(buffer.playout(start, output) == PlayoutResult::audio);
    REQUIRE(buffer.playing());
    REQUIRE(output_equals(output, 1.0f));
}

TEST_CASE("Jitter buffer can move its startup anchor backwards", "[jitter] [startup]") {
    TestBuffer buffer{800, 3};

    std::array<float, 4> output{};
    const auto start = TimePoint{};

    REQUIRE(buffer.insert(make_packet(12, 3.0f)) == InsertResult::accepted);
    REQUIRE(buffer.insert(make_packet(13, 4.0f)) == InsertResult::accepted);

    REQUIRE(buffer.insert(make_packet(10, 1.0f)) == InsertResult::accepted);
    REQUIRE(buffer.insert(make_packet(11, 2.0f)) == InsertResult::accepted);

    REQUIRE(buffer.playout(start, output) == PlayoutResult::audio);
    REQUIRE(output_equals(output, 1.0f));
    REQUIRE(buffer.expected_sequence() == 11);
}

TEST_CASE("Jitter buffer starts with the configured startup threshold of one", "[jitter] [startup]") {
    TestBuffer buffer{800, 1};

    std::array<float, 4> output{};
    const auto start = TimePoint{};

    REQUIRE(buffer.insert(make_packet(42, 5.0f)) == InsertResult::accepted);

    REQUIRE(buffer.playout(start, output) == PlayoutResult::audio);
    REQUIRE(buffer.playing());
    REQUIRE(output_equals(output, 5.0f));
}

// -----------------------------------------------------------------------------
// Packet insertion
// -----------------------------------------------------------------------------

TEST_CASE("Jitter buffer accepts packets within its capacity window", "[jitter] [insertion]") {
    TestBuffer buffer{800, 4};

    for (std::uint32_t sequence = 10; sequence < 18; ++sequence) {
        REQUIRE(buffer.insert(make_packet(sequence, static_cast<float>(sequence)))
                == InsertResult::accepted);
    }

    REQUIRE(buffer.buffered_packets() == 8);
}

TEST_CASE("Jitter buffer rejects packets beyond its capacity window", "[jitter] [insertion]") {
    TestBuffer buffer{800, 4};

    REQUIRE(buffer.insert(make_packet(10, 1.0f)) == InsertResult::accepted);

    // Capacity is eight, so sequence 18 is outside the valid window.
    REQUIRE(buffer.insert(make_packet(18, 2.0f)) == InsertResult::too_far_ahead);
    REQUIRE(buffer.buffered_packets() == 1);
}

TEST_CASE("Jitter buffer rejects duplicate packets", "[jitter] [insertion]") {
    TestBuffer buffer{800, 2};

    REQUIRE(buffer.insert(make_packet(10, 1.0f)) == InsertResult::accepted);
    REQUIRE(buffer.insert(make_packet(10, 2.0f)) == InsertResult::duplicate);

    REQUIRE(buffer.buffered_packets() == 1);
}

TEST_CASE("Jitter buffer preserves the first copy of a duplicate packet", "[jitter] [insertion]") {
    TestBuffer buffer{800, 1};

    std::array<float, 4> output{};
    const auto start = TimePoint{};

    REQUIRE(buffer.insert(make_packet(10, 1.0f)) == InsertResult::accepted);
    REQUIRE(buffer.insert(make_packet(10, 9.0f)) == InsertResult::duplicate);

    REQUIRE(buffer.playout(start, output) == PlayoutResult::audio);
    REQUIRE(output_equals(output, 1.0f));
}

TEST_CASE("Jitter buffer accepts out-of-order packets", "[jitter] [insertion]") {
    TestBuffer buffer{800, 3};

    REQUIRE(buffer.insert(make_packet(12, 3.0f)) == InsertResult::accepted);
    REQUIRE(buffer.insert(make_packet(10, 1.0f)) == InsertResult::accepted);
    REQUIRE(buffer.insert(make_packet(11, 2.0f)) == InsertResult::accepted);

    REQUIRE(buffer.buffered_packets() == 3);
}

TEST_CASE("Jitter buffer rejects packets older than the startup window", "[jitter] [insertion]") {
    TestBuffer buffer{800, 2};

    REQUIRE(buffer.insert(make_packet(10, 1.0f)) == InsertResult::accepted);

    // More than Capacity packets behind the current startup anchor.
    REQUIRE(buffer.insert(make_packet(1, 2.0f)) == InsertResult::too_old);
}

TEST_CASE("Jitter buffer reports its buffered packet count", "[jitter] [insertion]") {
    TestBuffer buffer{800, 3};

    REQUIRE(buffer.buffered_packets() == 0);

    REQUIRE(buffer.insert(make_packet(10, 1.0f)) == InsertResult::accepted);
    REQUIRE(buffer.buffered_packets() == 1);

    REQUIRE(buffer.insert(make_packet(12, 3.0f)) == InsertResult::accepted);
    REQUIRE(buffer.buffered_packets() == 2);

    REQUIRE(buffer.insert(make_packet(11, 2.0f)) == InsertResult::accepted);
    REQUIRE(buffer.buffered_packets() == 3);
}

// -----------------------------------------------------------------------------
// Playout ordering
// -----------------------------------------------------------------------------

TEST_CASE("Jitter buffer plays packets in sequence order", "[jitter] [playout]") {
    TestBuffer buffer{800, 3};

    std::array<float, 4> output{};
    const auto start = TimePoint{};

    REQUIRE(buffer.insert(make_packet(12, 3.0f)) == InsertResult::accepted);
    REQUIRE(buffer.insert(make_packet(10, 1.0f)) == InsertResult::accepted);
    REQUIRE(buffer.insert(make_packet(11, 2.0f)) == InsertResult::accepted);

    REQUIRE(buffer.playout(start, output) == PlayoutResult::audio);
    REQUIRE(output_equals(output, 1.0f));
    REQUIRE(buffer.expected_sequence() == 11);

    REQUIRE(buffer.playout(start + 5ms, output) == PlayoutResult::audio);
    REQUIRE(output_equals(output, 2.0f));
    REQUIRE(buffer.expected_sequence() == 12);

    REQUIRE(buffer.playout(start + 10ms, output) == PlayoutResult::audio);
    REQUIRE(output_equals(output, 3.0f));
    REQUIRE(buffer.expected_sequence() == 13);
}

TEST_CASE("Jitter buffer advances expected sequence after successful playout", "[jitter] [playout]") {
    TestBuffer buffer{800, 1};

    std::array<float, 4> output{};
    const auto start = TimePoint{};

    REQUIRE(buffer.insert(make_packet(20, 1.0f)) == InsertResult::accepted);
    REQUIRE(buffer.playout(start, output) == PlayoutResult::audio);

    REQUIRE(buffer.expected_sequence() == 21);

    REQUIRE(buffer.insert(make_packet(21, 2.0f)) == InsertResult::accepted);
    REQUIRE(buffer.playout(start + 5ms, output) == PlayoutResult::audio);

    REQUIRE(buffer.expected_sequence() == 22);
}

// -----------------------------------------------------------------------------
// Playout timing
// -----------------------------------------------------------------------------

TEST_CASE("Jitter buffer plays the first packet immediately when startup completes", "[jitter] [timing]") {
    TestBuffer buffer{800, 1};

    std::array<float, 4> output{};
    const auto start = TimePoint{};

    REQUIRE(buffer.insert(make_packet(10, 1.0f)) == InsertResult::accepted);

    REQUIRE(buffer.playout(start, output) == PlayoutResult::audio);
    REQUIRE(output_equals(output, 1.0f));

    REQUIRE(buffer.next_deadline().has_value());
    REQUIRE(*buffer.next_deadline() == start + 5ms);
}

TEST_CASE("Jitter buffer does not play before the next deadline", "[jitter] [timing]") {
    TestBuffer buffer{800, 2};

    std::array<float, 4> output{};
    const auto start = TimePoint{};

    REQUIRE(buffer.insert(make_packet(10, 1.0f)) == InsertResult::accepted);
    REQUIRE(buffer.insert(make_packet(11, 2.0f)) == InsertResult::accepted);

    REQUIRE(buffer.playout(start, output) == PlayoutResult::audio);
    REQUIRE(output_equals(output, 1.0f));

    REQUIRE(buffer.playout(start + 4ms, output) == PlayoutResult::not_due);
    REQUIRE(buffer.expected_sequence() == 11);
    REQUIRE(*buffer.next_deadline() == start + 5ms);
}

TEST_CASE("Jitter buffer plays at the exact packet deadline", "[jitter] [timing]") {
    TestBuffer buffer{800, 2};

    std::array<float, 4> output{};
    const auto start = TimePoint{};

    REQUIRE(buffer.insert(make_packet(10, 1.0f)) == InsertResult::accepted);
    REQUIRE(buffer.insert(make_packet(11, 2.0f)) == InsertResult::accepted);

    REQUIRE(buffer.playout(start, output) == PlayoutResult::audio);
    REQUIRE(buffer.playout(start + 5ms, output) == PlayoutResult::audio);

    REQUIRE(output_equals(output, 2.0f));
    REQUIRE(buffer.expected_sequence() == 12);
}

TEST_CASE("Jitter buffer maintains an absolute playout schedule", "[jitter] [timing]") {
    TestBuffer buffer{800, 4};

    std::array<float, 4> output{};
    const auto start = TimePoint{};

    for (std::uint32_t sequence = 10; sequence < 14; ++sequence) {
        REQUIRE(buffer.insert(make_packet(sequence, static_cast<float>(sequence)))
                == InsertResult::accepted);
    }

    REQUIRE(buffer.playout(start, output) == PlayoutResult::audio);
    REQUIRE(*buffer.next_deadline() == start + 5ms);

    REQUIRE(buffer.playout(start + 5ms, output) == PlayoutResult::audio);
    REQUIRE(*buffer.next_deadline() == start + 10ms);

    REQUIRE(buffer.playout(start + 10ms, output) == PlayoutResult::audio);
    REQUIRE(*buffer.next_deadline() == start + 15ms);

    REQUIRE(buffer.playout(start + 15ms, output) == PlayoutResult::audio);
    REQUIRE(*buffer.next_deadline() == start + 20ms);
}

TEST_CASE("Jitter buffer does not advance its deadline when playout is early", "[jitter] [timing]") {
    TestBuffer buffer{800, 2};

    std::array<float, 4> output{};
    const auto start = TimePoint{};

    REQUIRE(buffer.insert(make_packet(10, 1.0f)) == InsertResult::accepted);
    REQUIRE(buffer.insert(make_packet(11, 2.0f)) == InsertResult::accepted);

    REQUIRE(buffer.playout(start, output) == PlayoutResult::audio);

    const auto deadline = buffer.next_deadline();

    REQUIRE(buffer.playout(start + 1ms, output) == PlayoutResult::not_due);
    REQUIRE(buffer.playout(start + 2ms, output) == PlayoutResult::not_due);
    REQUIRE(buffer.playout(start + 3ms, output) == PlayoutResult::not_due);

    REQUIRE(buffer.next_deadline() == deadline);
}

// -----------------------------------------------------------------------------
// Packet loss and concealment
// -----------------------------------------------------------------------------

TEST_CASE("Jitter buffer conceals a missing packet at its deadline", "[jitter] [concealment]") {
    TestBuffer buffer{800, 2};

    std::array<float, 4> output{};
    const auto start = TimePoint{};

    REQUIRE(buffer.insert(make_packet(10, 1.0f)) == InsertResult::accepted);
    REQUIRE(buffer.insert(make_packet(11, 2.0f)) == InsertResult::accepted);

    REQUIRE(buffer.playout(start, output) == PlayoutResult::audio);
    REQUIRE(output_equals(output, 1.0f));

    REQUIRE(buffer.playout(start + 5ms, output) == PlayoutResult::audio);
    REQUIRE(output_equals(output, 2.0f));

    // Packet 12 never arrived.
    REQUIRE(buffer.playout(start + 10ms, output) == PlayoutResult::concealed);
    REQUIRE(output_equals(output, 0.0f));
    REQUIRE(buffer.expected_sequence() == 13);
}

TEST_CASE("Jitter buffer continues playing after concealing a missing packet", "[jitter] [concealment]") {
    TestBuffer buffer{800, 2};

    std::array<float, 4> output{};
    const auto start = TimePoint{};

    REQUIRE(buffer.insert(make_packet(10, 1.0f)) == InsertResult::accepted);
    REQUIRE(buffer.insert(make_packet(11, 2.0f)) == InsertResult::accepted);
    REQUIRE(buffer.insert(make_packet(13, 4.0f)) == InsertResult::accepted);

    REQUIRE(buffer.playout(start, output) == PlayoutResult::audio);
    REQUIRE(output_equals(output, 1.0f));

    REQUIRE(buffer.playout(start + 5ms, output) == PlayoutResult::audio);
    REQUIRE(output_equals(output, 2.0f));

    REQUIRE(buffer.playout(start + 10ms, output) == PlayoutResult::concealed);
    REQUIRE(output_equals(output, 0.0f));

    REQUIRE(buffer.playout(start + 15ms, output) == PlayoutResult::audio);
    REQUIRE(output_equals(output, 4.0f));

    REQUIRE(buffer.expected_sequence() == 14);
}

TEST_CASE("Jitter buffer rejects a packet that arrives after its playout deadline", "[jitter] [concealment]") {
    TestBuffer buffer{800, 2};

    std::array<float, 4> output{};
    const auto start = TimePoint{};

    REQUIRE(buffer.insert(make_packet(10, 1.0f)) == InsertResult::accepted);
    REQUIRE(buffer.insert(make_packet(11, 2.0f)) == InsertResult::accepted);

    REQUIRE(buffer.playout(start, output) == PlayoutResult::audio);
    REQUIRE(buffer.playout(start + 5ms, output) == PlayoutResult::audio);

    REQUIRE(buffer.playout(start + 10ms, output) == PlayoutResult::concealed);

    // Sequence 12 is now too late to be played.
    REQUIRE(buffer.insert(make_packet(12, 3.0f)) == InsertResult::too_old);
}

TEST_CASE("Jitter buffer zero-fills concealed output", "[jitter] [concealment]") {
    TestBuffer buffer{800, 1};

    std::array<float, 4> output{};
    const auto start = TimePoint{};

    REQUIRE(buffer.insert(make_packet(10, 1.0f)) == InsertResult::accepted);

    REQUIRE(buffer.playout(start, output) == PlayoutResult::audio);

    output.fill(123.0f);

    REQUIRE(buffer.playout(start + 5ms, output) == PlayoutResult::concealed);
    REQUIRE(output_equals(output, 0.0f));
}

// -----------------------------------------------------------------------------
// Late worker scheduling
// -----------------------------------------------------------------------------

TEST_CASE("Jitter buffer skips overdue packets instead of bursting them", "[jitter] [scheduling]") {
    TestBuffer buffer{800, 6};

    std::array<float, 4> output{};
    const auto start = TimePoint{};

    for (std::uint32_t sequence = 10; sequence < 16; ++sequence) {
        REQUIRE(buffer.insert(make_packet(sequence, static_cast<float>(sequence)))
                == InsertResult::accepted);
    }

    REQUIRE(buffer.playout(start, output) == PlayoutResult::audio);
    REQUIRE(output_equals(output, 10.0f));

    // Deadlines for packets 11 and 12 have passed.
    REQUIRE(buffer.playout(start + 15ms, output) == PlayoutResult::audio);
    REQUIRE(output_equals(output, 13.0f));

    REQUIRE(buffer.expected_sequence() == 14);
    REQUIRE(*buffer.next_deadline() == start + 20ms);
}

TEST_CASE("Jitter buffer conceals the current packet after skipping overdue packets", "[jitter] [scheduling]") {
    TestBuffer buffer{800, 1};

    std::array<float, 4> output{};
    const auto start = TimePoint{};

    REQUIRE(buffer.insert(make_packet(10, 10.0f)) == InsertResult::accepted);
    REQUIRE(buffer.insert(make_packet(14, 14.0f)) == InsertResult::accepted);

    REQUIRE(buffer.playout(start, output) == PlayoutResult::audio);
    REQUIRE(output_equals(output, 10.0f));

    // At 15 ms, packets 11 and 12 are overdue.
    // Packet 13 is now due, but it never arrived.
    REQUIRE(buffer.playout(start + 15ms, output) == PlayoutResult::concealed);
    REQUIRE(output_equals(output, 0.0f));

    REQUIRE(buffer.expected_sequence() == 14);

    // Packet 14 is due at 20 ms.
    REQUIRE(buffer.playout(start + 20ms, output) == PlayoutResult::audio);
    REQUIRE(output_equals(output, 14.0f));

    REQUIRE(buffer.expected_sequence() == 15);
}

TEST_CASE("Jitter buffer skips multiple overdue periods", "[jitter] [scheduling]") {
    TestBuffer buffer{800, 8};

    std::array<float, 4> output{};
    const auto start = TimePoint{};

    for (std::uint32_t sequence = 10; sequence < 18; ++sequence) {
        REQUIRE(buffer.insert(make_packet(sequence, static_cast<float>(sequence)))
                == InsertResult::accepted);
    }

    REQUIRE(buffer.playout(start, output) == PlayoutResult::audio);

    // Six packet periods have elapsed since the initial playout.
    REQUIRE(buffer.playout(start + 30ms, output) == PlayoutResult::audio);
    REQUIRE(output_equals(output, 16.0f));
    REQUIRE(buffer.expected_sequence() == 17);
    REQUIRE(*buffer.next_deadline() == start + 35ms);
}

// -----------------------------------------------------------------------------
// Sequence number wraparound
// -----------------------------------------------------------------------------

TEST_CASE("Jitter buffer accepts sequence numbers across uint32 wraparound", "[jitter] [sequence]") {
    TestBuffer buffer{800, 3};

    const auto first = UINT32_MAX - 1;

    REQUIRE(buffer.insert(make_packet(first, 1.0f)) == InsertResult::accepted);
    REQUIRE(buffer.insert(make_packet(UINT32_MAX, 2.0f)) == InsertResult::accepted);
    REQUIRE(buffer.insert(make_packet(0, 3.0f)) == InsertResult::accepted);
}

TEST_CASE("Jitter buffer plays sequence numbers across uint32 wraparound", "[jitter] [sequence]") {
    TestBuffer buffer{800, 3};

    std::array<float, 4> output{};
    const auto start = TimePoint{};

    REQUIRE(buffer.insert(make_packet(UINT32_MAX - 1, 1.0f)) == InsertResult::accepted);
    REQUIRE(buffer.insert(make_packet(0, 3.0f)) == InsertResult::accepted);
    REQUIRE(buffer.insert(make_packet(UINT32_MAX, 2.0f)) == InsertResult::accepted);

    REQUIRE(buffer.playout(start, output) == PlayoutResult::audio);
    REQUIRE(output_equals(output, 1.0f));
    REQUIRE(buffer.expected_sequence() == UINT32_MAX);

    REQUIRE(buffer.playout(start + 5ms, output) == PlayoutResult::audio);
    REQUIRE(output_equals(output, 2.0f));
    REQUIRE(buffer.expected_sequence() == 0);

    REQUIRE(buffer.playout(start + 10ms, output) == PlayoutResult::audio);
    REQUIRE(output_equals(output, 3.0f));
    REQUIRE(buffer.expected_sequence() == 1);
}

TEST_CASE("Jitter buffer rejects a wrapped sequence that is already too old", "[jitter] [sequence]") {
    TestBuffer buffer{800, 1};

    std::array<float, 4> output{};
    const auto start = TimePoint{};

    REQUIRE(buffer.insert(make_packet(UINT32_MAX, 1.0f)) == InsertResult::accepted);
    REQUIRE(buffer.playout(start, output) == PlayoutResult::audio);

    REQUIRE(buffer.insert(make_packet(UINT32_MAX, 2.0f)) == InsertResult::too_old);
}

// -----------------------------------------------------------------------------
// Slot reuse
// -----------------------------------------------------------------------------

TEST_CASE("Jitter buffer reuses slots after successful playout", "[jitter] [slots]") {
    TestBuffer buffer{800, 1};

    std::array<float, 4> output{};
    const auto start = TimePoint{};

    REQUIRE(buffer.insert(make_packet(10, 1.0f)) == InsertResult::accepted);
    REQUIRE(buffer.playout(start, output) == PlayoutResult::audio);

    REQUIRE(buffer.insert(make_packet(11, 2.0f)) == InsertResult::accepted);
    REQUIRE(buffer.buffered_packets() == 1);

    REQUIRE(buffer.playout(start + 5ms, output) == PlayoutResult::audio);
    REQUIRE(output_equals(output, 2.0f));
}

TEST_CASE("Jitter buffer reuses slots after a concealed packet", "[jitter] [slots]") {
    TestBuffer buffer{800, 1};

    std::array<float, 4> output{};
    const auto start = TimePoint{};

    REQUIRE(buffer.insert(make_packet(10, 1.0f)) == InsertResult::accepted);
    REQUIRE(buffer.playout(start, output) == PlayoutResult::audio);

    REQUIRE(buffer.playout(start + 5ms, output) == PlayoutResult::concealed);

    REQUIRE(buffer.insert(make_packet(12, 3.0f)) == InsertResult::accepted);
    REQUIRE(buffer.buffered_packets() == 1);

    REQUIRE(buffer.playout(start + 10ms, output) == PlayoutResult::audio);
    REQUIRE(output_equals(output, 3.0f));
}

// -----------------------------------------------------------------------------
// Reset
// -----------------------------------------------------------------------------

TEST_CASE("Jitter buffer reset clears all buffered packets", "[jitter] [reset]") {
    TestBuffer buffer{800, 3};

    REQUIRE(buffer.insert(make_packet(10, 1.0f)) == InsertResult::accepted);
    REQUIRE(buffer.insert(make_packet(11, 2.0f)) == InsertResult::accepted);

    REQUIRE(buffer.buffered_packets() == 2);

    buffer.reset();

    REQUIRE(buffer.buffered_packets() == 0);
    REQUIRE_FALSE(buffer.playing());
    REQUIRE_FALSE(buffer.next_deadline().has_value());
}

TEST_CASE("Jitter buffer reset returns it to startup state", "[jitter] [reset]") {
    TestBuffer buffer{800, 2};

    std::array<float, 4> output{};
    const auto start = TimePoint{};

    REQUIRE(buffer.insert(make_packet(10, 1.0f)) == InsertResult::accepted);
    REQUIRE(buffer.insert(make_packet(11, 2.0f)) == InsertResult::accepted);

    REQUIRE(buffer.playout(start, output) == PlayoutResult::audio);
    REQUIRE(buffer.playing());

    buffer.reset();

    REQUIRE_FALSE(buffer.playing());
    REQUIRE_FALSE(buffer.next_deadline().has_value());

    REQUIRE(buffer.insert(make_packet(50, 5.0f)) == InsertResult::accepted);
    REQUIRE(buffer.insert(make_packet(51, 6.0f)) == InsertResult::accepted);

    REQUIRE(buffer.playout(start, output) == PlayoutResult::audio);
    REQUIRE(output_equals(output, 5.0f));
    REQUIRE(buffer.expected_sequence() == 51);
}

TEST_CASE("Jitter buffer reset allows a new sequence number range", "[jitter] [reset]") {
    TestBuffer buffer{800, 1};

    std::array<float, 4> output{};
    const auto start = TimePoint{};

    REQUIRE(buffer.insert(make_packet(1000, 1.0f)) == InsertResult::accepted);
    REQUIRE(buffer.playout(start, output) == PlayoutResult::audio);

    buffer.reset();

    REQUIRE(buffer.insert(make_packet(5, 2.0f)) == InsertResult::accepted);
    REQUIRE(buffer.playout(start, output) == PlayoutResult::audio);
    REQUIRE(output_equals(output, 2.0f));
}