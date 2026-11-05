#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <thread>

#include "audio/sink.hpp"

namespace {
    constexpr uint32_t sample_rate = 48'000;
    constexpr std::size_t sample_size = 240;
    constexpr float frequency = 1000.0f;
}

int main() {
    sf::audio_sink<sample_size> sink;

    std::cout << "Opening audio sink...\n";
    if(sink.open(sample_rate) < 0) {
        std::cerr << "Could not open audio sink!\n";
        return 1;
    }
    
    std::thread producer = std::thread([&] {
        double phase = 0.0;
        const double phase_step = 
            2.0 * M_PI * frequency / sample_rate;

        auto deadline = std::chrono::steady_clock::now();

        // 1000 * 240 at 48 kHz should produce 5 seconds of audio
        for(std::size_t i = 0; i < 1000; i++) {
            std::array<float, sample_size> samples;
            for(std::size_t j = 0; j < sample_size; j++) {
                samples[j] = 0.15f * std::sin(phase);
                phase += phase_step;

                if(phase >= 2.0 * M_PI) {
                    phase -= 2.0 * M_PI;
                }
            }
            sink.write(samples);
            
            deadline += std::chrono::milliseconds(5);
            std::this_thread::sleep_until(deadline);
        }
    });

    producer.join();

    std::cout << "Closing audio sink...\n";
    sink.close();
    
    return 0;
}