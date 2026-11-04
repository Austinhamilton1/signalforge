#pragma once

#if defined(_WIN32)
    #include <winsock2.h>
    #pragma comment(lib, "Ws2_32.lib")
#else
    #include <arpa/inet.h>
#endif

#include <iostream>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "protocol.hpp"

namespace {
    uint64_t htonll(uint64_t value) {
        if constexpr (std::endian::native == std::endian::little) {
            // Swap bytes if the native architecture is little-endian.
            return std::byteswap(value);
        }
        return value; // Already in network byte order (big-endian).
    }

    uint64_t ntohll(uint64_t value) {
        if constexpr (std::endian::native == std::endian::little) {
            // Swap bytes if the native architecture is little-endian.
            return std::byteswap(value);
        }
        return value; // Already in host byte order.
    }

    uint32_t htonf(float value) {
        uint32_t result;
        std::memcpy(&result, &value, sizeof(result));
        return htonl(result);
    }

    float ntohf(uint32_t value) {
        uint32_t result = ntohl(value);
        float float_result;
        std::memcpy(&float_result, &result, sizeof(float_result));
        return float_result;
    }
}

namespace sf {
    class serializer {
    public:
        /*
         * Serialize an audio packet into a byte buffer.
         *
         * @param buffer The byte buffer to write the serialized data into.
         * @param packet The audio packet to serialize.
         * @param buffer_size The size of the byte buffer.
         * 
         * @return Number of bytes serialized.
         */
        static size_t serialize(uint8_t* buffer, const audio_packet_t& packet, size_t buffer_size) {
            if(packet.header.frame_count > detail::frames_per_packet) {
                return 0; // Frame count exceeds maximum allowed.
            }

            if(
                buffer_size < sizeof(packet.header.seq) 
                + sizeof(packet.header.timestamp) 
                + sizeof(packet.header.frame_count) 
                + sizeof(packet.header.flags) 
                + packet.header.frame_count * sizeof(float)
            ) {
                return 0; // Buffer size is insufficient.
            }
            
            uint32_t seq = htonl(packet.header.seq);
            uint64_t timestamp = htonll(packet.header.timestamp);
            uint16_t frame_count = htons(packet.header.frame_count);
            uint16_t flags = htons(packet.header.flags);

            std::memcpy(buffer, &seq, sizeof(seq));
            std::memcpy(buffer + sizeof(seq), &timestamp, sizeof(timestamp));
            std::memcpy(buffer + sizeof(seq) + sizeof(timestamp), &frame_count, sizeof(frame_count));
            std::memcpy(buffer + sizeof(seq) + sizeof(timestamp) + sizeof(frame_count), &flags, sizeof(flags));
            
            for(size_t i = 0; i < packet.header.frame_count; i++) {
                uint32_t sample = htonf(packet.samples[i]);
                std::memcpy(buffer + sizeof(seq) + sizeof(timestamp) + sizeof(frame_count) + sizeof(flags) + i * sizeof(sample), &sample, sizeof(sample));
            }

            return sizeof(packet.header.seq)
                + sizeof(packet.header.timestamp)
                + sizeof(packet.header.frame_count)
                + sizeof(packet.header.flags)
                + packet.header.frame_count * sizeof(float);
        }

        /*
         * Deserialize an audio packet from a byte buffer.
         *
         * @param packet The audio packet to populate with the deserialized data.
         * @param buffer The byte buffer containing the serialized data.
         * @param buffer_size The size of the byte buffer.
         * 
         * @return Size of resulting packet.
         */
        static size_t deserialize(audio_packet_t& packet, const uint8_t* buffer, size_t buffer_size) {
            uint32_t seq;
            uint64_t timestamp;
            uint16_t frame_count;
            uint16_t flags;

            if(
                buffer_size < sizeof(seq) 
                + sizeof(timestamp) 
                + sizeof(frame_count) 
                + sizeof(flags)
            ) {
                return 0; // Buffer size is insufficient.
            }

            std::memcpy(&seq, buffer, sizeof(seq));
            std::memcpy(&timestamp, buffer + sizeof(seq), sizeof(timestamp));
            std::memcpy(&frame_count, buffer + sizeof(seq) + sizeof(timestamp), sizeof(frame_count));
            std::memcpy(&flags, buffer + sizeof(seq) + sizeof(timestamp) + sizeof(frame_count), sizeof(flags));
            
            packet.header.seq = ntohl(seq);
            packet.header.timestamp = ntohll(timestamp);
            packet.header.frame_count = ntohs(frame_count);
            packet.header.flags = ntohs(flags);

            if(packet.header.frame_count > detail::frames_per_packet) {
                return 0; // Frame count exceeds maximum allowed.
            }

            if(
                buffer_size < sizeof(seq) + 
                sizeof(timestamp) + 
                sizeof(frame_count) + 
                sizeof(flags) + 
                packet.header.frame_count * sizeof(float)
            ) {
                return 0; // Buffer size is insufficient for the samples.
            }

            for(size_t i = 0; i < packet.header.frame_count; i++) {
                uint32_t sample;
                std::memcpy(&sample, buffer + sizeof(seq) + sizeof(timestamp) + sizeof(frame_count) + sizeof(flags) + i * sizeof(sample), sizeof(sample));
                packet.samples[i] = ntohf(sample);
            }

            return sizeof(packet.header.seq)
                + sizeof(packet.header.timestamp)
                + sizeof(packet.header.frame_count)
                + sizeof(packet.header.flags)
                + packet.header.frame_count * sizeof(float);
        }
    };
}