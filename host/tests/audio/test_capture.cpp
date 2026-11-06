#include <csignal>
#include <cstdint>
#include <iostream>
#include <thread>

#include "audio/capture.hpp"
#include "data/channel.hpp"

namespace {
    constexpr uint32_t sample_rate = 48'000;
}

std::atomic<bool> shutdown_requested{false};

extern "C" void signal_handler(int signal) {
    if(signal == SIGINT || signal == SIGTERM) {
        shutdown_requested.store(true);
    }
}

int main() {
    sf::audio_capture capture;
    sf::channel<float> pipe(4096);

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "Opening audio capture...\n";
    if(capture.open(sample_rate, pipe) < 0) {
        std::cerr << "Could not open audio capture!\n";
        return 1;
    }

    // Wait for a signal to shutdown the process
    while(!shutdown_requested.load()) { std::this_thread::yield(); }

    std::cout << "Closing audio capture.\n";
    capture.close();

    return 0;
}