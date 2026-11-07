#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <thread>

#include "audio/capture.hpp"
#include "audio/sink.hpp"
#include "data/channel.hpp"

namespace {
    constexpr size_t frame_size = 240;
    constexpr uint32_t sample_rate = 48'000;
}

std::atomic<bool> shutdown_requested{false};

extern "C" void signal_handler(int signal) {
    if(signal == SIGTERM || signal == SIGINT) {
        shutdown_requested.store(true);
    }
}

int main() {
    sf::channel<float> pipe(4096);

    sf::audio_sink sink;
    sf::audio_capture capture;

    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT, signal_handler);

    std::cout << "Opening audio sink\n";
    if(sink.open(sample_rate) < 0) {
        std::cerr << "Could not open audio sink\n";
        return 1;
    }

    std::cout << "Opening audio capture\n";
    if(capture.open(sample_rate, pipe) < 0) {
        std::cerr << "Could not open audio capture\n";
        sink.close();
        return 1;
    }

    auto produce_audio = std::thread([&] {
        std::array<float, frame_size> captured_frames;

        while(!shutdown_requested.load()) {
            int result = pipe.wait_n(
                captured_frames.data(),
                frame_size,
                5,
                [&](float *samples) {
                    sink.write(samples, frame_size);
                }
            );

            if(result < 0) {
                break;
            }
        }
    });

    produce_audio.join();

    capture.close();
    sink.close();

    return 0;
}