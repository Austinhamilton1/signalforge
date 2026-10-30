#pragma once

#include <cstddef>
#include <cstdint>

namespace sf {
    namespace detail {
        // At 48K this translates to ~5ms of audio
        constexpr size_t frames_per_packet = 240;
    }
    
    struct audio_packet_header_t {
        uint32_t seq;
        uint64_t timestamp;
        uint16_t frame_count;
        uint16_t flags;
    };
    
    struct audio_packet_t {
        audio_packet_header_t header;
        float samples[detail::frames_per_packet];
    };
}