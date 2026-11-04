#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

#include "data/channel.hpp"
#include "network/protocol.hpp"
#include "network/server.hpp"

namespace {
    constexpr int port = 8080;
    constexpr size_t num_packets = 1024;

    struct Recorder {
    public:
        explicit Recorder(uint32_t max_seq) {
            for(uint32_t i = 0; i < max_seq; i++) {
                m_lookup[i] = {0};
            }
        }

        void record(uint32_t seq) {
            m_lookup[seq].fetch_add(1, std::memory_order_relaxed);
        }

        bool check_count(size_t n) {
            for(const auto& it : m_lookup) {
                if(it.second != n)
                    return false;
            }

            return true;
        }

    private: 
        std::unordered_map<uint32_t, std::atomic<size_t>> m_lookup;
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
        while(!server.running()) {
            std::this_thread::yield();
        }

        server.stop();
    });

    serve.join();
    stop.join();

    // Arbitrary true to ensure code got to this point
    REQUIRE(1 == 1);
}

TEST_CASE("Server can send and receive data", "[server] [behavior]") {
    sf::server server;
    Recorder recorder(static_cast<uint32_t>(num_packets));
    REQUIRE(server.init() == 0);

    sf::channel<sf::audio_packet_t> pipe(1024);

    int epoll_fd = epoll_create1(0);
    REQUIRE(epoll_fd >= 0);

    struct epoll_event pipe_ev;
    pipe_ev.events = EPOLLIN;
    pipe_ev.data.fd = pipe.fd();

    REQUIRE(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, pipe.fd(), &pipe_ev) >= 0);

    std::thread serve([&] {
        server.listen(port, pipe);
    });

    std::thread producer([&] {
        // Wait for the server to start running.
        while(!server.running()) {
            std::this_thread::yield();
        }

        for(size_t i = 0; i < num_packets; i++) {
            sf::audio_packet_t packet;
            packet.header.seq = i;
            packet.header.timestamp = sf::detail::frames_per_packet * i;
            packet.header.frame_count = sf::detail::frames_per_packet;
            packet.header.flags = 0x1;

            for(size_t j = 0; j < packet.header.frame_count; j++) {
                packet.samples[j] = static_cast<float>(j);
            }

            server.send("127.0.0.1", port, packet);
        }
    });

    std::thread consumer([&] {
        // Wait for the server to start running.
        while(!server.running()) {
            std::this_thread::yield();
        }

        auto start = std::chrono::steady_clock::now();
        auto end = start;

        // Ensure that the consumer loop stops after 2000 ms (timeout)
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        size_t received = 0;

        while(duration < 2000 && received < num_packets) {
            struct epoll_event ev;
            // Time out after 2000 ms to match the consumer loop timeout.
            int nfds = epoll_wait(epoll_fd, &ev, 1, 2000);
            if(nfds < 0) {
                if(errno == EINTR) {
                    continue;
                }
                break;
            }

            for(int n = 0; n < nfds; n++) {
                if(ev.data.fd == pipe.fd()) {
                    uint64_t signals;
                    read(pipe.fd(), &signals, sizeof(signals));

                    while(auto *packet = pipe.buffer().read()) {
                        recorder.record(packet->header.seq);
                        pipe.buffer().release();

                        received++;
                    }
                }
            }

            end = std::chrono::steady_clock::now();
            duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        }

        server.stop();
    });

    serve.join();
    producer.join();
    consumer.join();

    close(epoll_fd);

    REQUIRE(recorder.check_count(1));
}