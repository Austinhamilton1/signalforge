#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <thread>
#include <unordered_map>

#include "data/channel.hpp"
#include "network/protocol.hpp"
#include "network/server.hpp"

namespace {
    constexpr int port = 8080;
    constexpr std::size_t num_packets = 1024;
    constexpr int timeout_ms = 2000;

    template<typename T>
    struct Recorder {
    public:
        explicit Recorder(std::size_t max_n) {
            for(std::size_t i = 0; i < max_n; i++) {
                m_lookup[static_cast<T>(i)] = {0};
            }
        }

        void record(T x) {
            m_lookup[x].fetch_add(1, std::memory_order_relaxed);
        }

        bool check_count(std::size_t n) {
            for(const auto& it : m_lookup) {
                if(it.second != n)
                    return false;
            }

            return true;
        }

    private: 
        std::unordered_map<T, std::atomic<std::size_t>> m_lookup;
    };
}

TEST_CASE("Server can be initialized without throwing", "[server] [init]") {
    sf::server server;
    REQUIRE_NOTHROW(server.init());
}

TEST_CASE("Server can be interrupted", "[server] [stop]") {
    sf::server server;
    REQUIRE(server.init() == 0);

    sf::channel<sf::audio_packet_t> pipe(4);

    std::thread serve([&] {
        server.listen(8080, pipe);
    });

    std::thread stop([&] {
        // Wait for the server to start
        server.wait();

        server.stop();
    });

    serve.join();
    stop.join();

    // Arbitrary true to ensure code got to this point
    REQUIRE(1 == 1);
}

TEST_CASE("Server can send and receive data", "[server] [behavior]") {
    sf::server server;
    Recorder<uint32_t> seq_recorder(num_packets);
    Recorder<float> sample_recorder(sf::detail::frames_per_packet);
    REQUIRE(server.init() == 0);

    sf::channel<sf::audio_packet_t> pipe(1024);

    std::thread serve([&] {
        server.listen(port, pipe);
    });

    std::thread producer([&] {
        // Wait for the server to start running.
        server.wait();

        for(std::size_t i = 0; i < num_packets; i++) {
            sf::audio_packet_t packet;
            packet.header.seq = i;
            packet.header.timestamp = sf::detail::frames_per_packet * i;
            packet.header.frame_count = sf::detail::frames_per_packet;
            packet.header.flags = 0x1;

            for(std::size_t j = 0; j < packet.header.frame_count; j++) {
                packet.samples[j] = static_cast<float>(j);
            }

            server.send("127.0.0.1", port, packet);
        }
    });

    std::thread consumer([&] {
        // Wait for the server to start running.
        server.wait();

        auto start = std::chrono::steady_clock::now();
        auto end = start;

        // Ensure that the consumer loop stops after 2000 ms (timeout)
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        std::size_t received = 0;

        while(duration < timeout_ms && received < num_packets) {
            if(pipe.wait(timeout_ms, [&](sf::audio_packet_t *packet) {
                seq_recorder.record(packet->header.seq);
                for(std::size_t i = 0; i < packet->header.frame_count; i++) {
                   sample_recorder.record(packet->samples[i]); 
                }
                received++;
            }) < 0) {
                break;
            }

            end = std::chrono::steady_clock::now();
            duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        }

        server.stop();
    });

    serve.join();
    producer.join();
    consumer.join();

    REQUIRE(seq_recorder.check_count(1));
    REQUIRE(sample_recorder.check_count(num_packets));
}