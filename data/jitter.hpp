#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace sf {
    template<size_t Frames, size_t Capacity>
    class jitter_buf {
        static_assert(Frames > 0);
        static_assert(Capacity > 0);
        static_assert(Capacity < 0x80000000UL);

    public:
        using clock_t = std::chrono::steady_clock;
        using time_point_t = clock_t::time_point;
        using duration_t = clock_t::duration;

        struct packet_t {
            // Sequence number of this packet.
            uint32_t seq = 0;

            // Sample counter of the first audio frame in this packet.
            uint64_t timestamp = 0;

            std::array<float, Frames> samples;
        };

        enum class insert_result_t {
            accepted,
            duplicate,
            too_old,
            too_far_ahead,
            slot_collision
        };

        enum class playout_result_t {
            buffering,
            not_due,
            audio,
            concealed
        };

        explicit jitter_buf(uint32_t sample_rate, size_t startup_packets = 4)
            : m_startup_packets(startup_packets) 
        {
            if(sample_rate == 0) {
                throw std::invalid_argument("Sample rate must be nonzero");
            }

            if(startup_packets == 0 || startup_packets > Capacity) {
                throw std::invalid_argument("Invalid jitter buffer startup size");
            }

            const std::chrono::duration<double> packet_duration(
                static_cast<double>(Frames) / sample_rate
            );

            m_packet_period =
                std::chrono::duration_cast<duration_t>(packet_duration);

            if(m_packet_period <= duration_t::zero()) {
                throw std::invalid_argument("Packet duration is too small");
            }
        }

        jitter_buf(const jitter_buf&) = delete;
        jitter_buf& operator=(const jitter_buf&) = delete;
        jitter_buf(jitter_buf&&) = delete;
        jitter_buf& operator=(jitter_buf&&) = delete;

        insert_result_t insert(const packet_t& packet) {
            const uint32_t sequence = packet.seq;

            if(!m_have_base) {
                m_base_sequence = sequence;
                m_have_base = true;
            }

            // Before playback begins, allow an earlier packet to move
            // the startup anchor backwards.
            if(!m_playing) {
                const auto from_base =
                    sequence_distance(sequence, m_base_sequence);

                if(from_base < 0) {
                    if(from_base <= -static_cast<int64_t>(Capacity)) {
                        return insert_result_t::too_old;
                    }

                    // Ensure all existing packets still fit inside 
                    // the window if we move the anchor backwards
                    for(const slot_t& slot : m_slots) {
                        if(!slot.occupied) {
                            continue;
                        }

                        const auto distance =
                            sequence_distance(slot.packet.seq, sequence);

                        if(distance < 0 || distance >= static_cast<int64_t>(Capacity)) {
                            return insert_result_t::too_far_ahead;
                        }
                    }

                    m_base_sequence = sequence;
                }
            }

            const uint32_t reference =
                m_playing ? m_expected_sequence : m_base_sequence;

            const auto distance =
                sequence_distance(sequence, reference);

            if(distance < 0) {
                return insert_result_t::too_old;
            }

            if(distance >= static_cast<int64_t>(Capacity)) {
                return insert_result_t::too_far_ahead;
            }

            slot_t& slot = m_slots[sequence % Capacity];

            if(slot.occupied) {
                if(slot.packet.seq == sequence) {
                    return insert_result_t::duplicate;
                }

                // A consumed packet can leave a stale slot behind.
                // It is safe to reuse that slot.
                if(m_playing && sequence_distance(slot.packet.seq, m_expected_sequence) < 0) {
                    slot.occupied = false;
                    m_buffered_packets--;
                } else {
                    return insert_result_t::slot_collision;
                }
            }

            slot.packet = packet;
            slot.occupied = true;
            m_buffered_packets++;

            return insert_result_t::accepted;
        }

        playout_result_t playout(time_point_t now, std::array<float, Frames>& output) {
            if(!m_playing) {
                if(!has_startup_run()) {
                    output.fill(0.0f);
                    return playout_result_t::buffering;
                }

                // Establish the playout timeline
                m_expected_sequence = m_base_sequence;
                m_next_deadline = now;
                m_playing = true;
            }

            if(now < m_next_deadline) {
                output.fill(0.0f);
                return playout_result_t::not_due;
            }

            // If the worker woke up late, skip expired packet slots.
            // Never emit several overdue packets in a burst.
            const auto late_periods =
                (now - m_next_deadline) / m_packet_period;

            if(late_periods > 0) {
                m_expected_sequence += 
                    static_cast<uint32_t>(late_periods);

                m_next_deadline += m_packet_period * late_periods;

                discard_packets_before(m_expected_sequence);
            }

            slot_t& slot = m_slots[m_expected_sequence % Capacity];

            playout_result_t result = playout_result_t::concealed;

            if(slot.occupied && slot.packet.seq == m_expected_sequence) {
                output = slot.packet.samples;
                slot.occupied = false;
                m_buffered_packets--;

                result = playout_result_t::audio;
            } else {
                // The packet missed its playout slot. Output silence
                // and advance anyway; don't wait for a late packet.
                output.fill(0.0f);
            }

            m_expected_sequence++;
            m_next_deadline += m_packet_period;

            return result;
        }

        [[nodiscard]]
        std::optional<time_point_t> next_deadline() const noexcept {
            if(!m_playing) {
                return std::nullopt;
            }
            
            return m_next_deadline;
        }

        [[nodiscard]]
        bool playing() const noexcept {
            return m_playing;
        }

        [[nodiscard]]
        size_t buffered_packets() const noexcept {
            return m_buffered_packets;
        }

        [[nodiscard]]
        uint32_t expected_sequence() const noexcept {
            return m_expected_sequence;
        }

        void reset() {
            for(slot_t& slot : m_slots) {
                slot.occupied = false;
            }

            m_have_base = false;
            m_playing = false;
            m_base_sequence = 0;
            m_expected_sequence = 0;
            m_buffered_packets = 0;
            m_next_deadline = time_point_t{};
        }

    private:
        struct slot_t {
            packet_t packet{};
            bool occupied = false;
        };

        /*
         * Returns the signed distance from 'base' to 'sequence'.
         * This handles uint32_t sequence-number wraparound, provided
         * distances remain less than 2^31 packets.
         */
        static int64_t sequence_distance(
            uint32_t sequence,
            uint32_t base
        ) noexcept {
            const uint32_t difference = sequence - base;

            if(difference < 0x80000000u) {
                return static_cast<int64_t>(difference);
            }

            return static_cast<int64_t>(difference) - 0x100000000LL;
        }

        [[nodiscard]]
        bool has_startup_run() const noexcept {
            if(!m_have_base) {
                return false;
            }

            // Require startup_packets consecutive sequence numbers.
            for(size_t i = 0; i < m_startup_packets; i++) {
                const uint32_t sequence =
                    m_base_sequence + static_cast<uint32_t>(i);

                const slot_t& slot = m_slots[sequence % Capacity];

                if(!slot.occupied || slot.packet.seq != sequence) {
                    return false;
                }
            }

            return true;
        }

        void discard_packets_before(uint32_t sequence) noexcept {
            for(slot_t& slot : m_slots) {
                if(!slot.occupied) {
                    continue;
                }

                if(sequence_distance(slot.packet.seq, sequence) < 0) {
                    slot.occupied = false;
                    m_buffered_packets--;
                }
            }
        }

        std::array<slot_t, Capacity> m_slots{};

        const size_t m_startup_packets;
        duration_t m_packet_period{};

        time_point_t m_next_deadline{};

        uint32_t m_base_sequence = 0;
        uint32_t m_expected_sequence = 0;

        size_t m_buffered_packets = 0;
    
        bool m_have_base = false;
        bool m_playing = false;
    };
}