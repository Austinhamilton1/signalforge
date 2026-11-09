#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <pthread.h>
#include <string>
#include <thread>
#include <unistd.h>

#include "audio/capture.hpp"
#include "audio/sink.hpp"
#include "data/channel.hpp"
#include "data/jitter.hpp"
#include "network/protocol.hpp"
#include "network/server.hpp"

namespace {
    constexpr std::size_t frames = sf::detail::frames_per_packet;
    constexpr std::size_t jitter_size = 32;

    using jitter_buf_t = sf::jitter_buf<frames, jitter_size>; 

    struct sf_args {
        bool invalid_argument = false;
        bool print_usage = false;
        int host_port = 8080;
        int remote_port = 8080;
        uint32_t sample_rate = 48'000;
        int buffer_delay = 60;
        std::string remote_addr = "";
        std::string error = "";
    };

    void usage() {
        std::cout << "===========================================================================\n";
        std::cout << "Usage: signalforge [options] [device IP]\n";
        std::cout << "Options:\n";
        std::cout << "    -h                    Print usage\n";
        std::cout << "    -s <port>             Specify source port (default 8080)\n";
        std::cout << "    -d <port>             Specify destination port (default 8080)\n";
        std::cout << "    -r <sample_rate>      Specify audio sample rate in Hz (default 48000)\n";
        std::cout << "    -b <startup_delay>    Specify packet buffering delay in ms (default 60)\n";
        std::cout << "===========================================================================\n";
    }

    int parse_args(int argc, char **argv, sf_args& args) {
        int opt;

        while((opt = getopt(argc, argv, "hs:d:r:b:")) != -1) {
            switch(opt) {
                case 'h':
                    args.print_usage = true;
                    break;
                case 's':
                    try {
                        args.host_port = std::stoi(optarg);
                    } catch(const std::exception&) {
                        std::cerr << "Error: -s requires a valid integer argument\n";
                        return -1;
                    }
                    break;
                case 'd':
                    try {
                        args.remote_port = std::stoi(optarg);
                    } catch(const std::exception&) {
                        std::cerr << "Error: -d requires a valid integer argument\n";
                        return -1;
                    }
                    break;
                case 'r':
                    try {
                        args.sample_rate = static_cast<uint32_t>(std::stoi(optarg));
                    } catch(const std::exception&) {
                        std::cerr << "Error: -s requires a valid integer argument\n";
                        return -1;
                    }
                    break;
                case 'b':
                    try {
                        args.buffer_delay = std::stoi(optarg);
                    } catch(const std::exception&) {
                        std::cerr << "Error: -b requires a valid integer argument\n";
                        return -1;
                    }
                    break;
                case '?':
                    args.invalid_argument = true;
                    break;
                default:
                    return -1;
            }
        }

        if(argv[optind]) {
            args.remote_addr = argv[optind];
        }

        return 0;
    }

    bool valid_args(sf_args& args) {
        if(args.invalid_argument) {
            args.error = "Invalid argument";
            return false;
        }

        if(args.print_usage) return true;

        if(args.host_port < 1 || args.host_port > 65535) {
            args.error = "Invalid port range";
            return false;
        }

        if(args.remote_port < 1 || args.remote_port > 65535) {
            args.error = "Invalid port range";
            return false;
        }

        if(args.sample_rate == 0) {
            args.error = "Invalid sample rate";
            return false;
        }

        if(args.buffer_delay <= 0) {
            args.error = "Invalid buffer delay";
            return false;
        }

        if(args.remote_addr.size() == 0) {
            args.error = "Missing remote address";
            return false;
        }

        return true;
    }

    int timeout_ms(jitter_buf_t& jitter) {
        auto deadline = jitter.next_deadline();

        if(!deadline) {
            return -1; // Wait indefinitely for startup packets.
        }

        auto now = jitter_buf_t::clock_t::now();

        if(now >= *deadline) {
            return 0;
        }

        auto remaining = *deadline - now;

        return static_cast<int>(
            std::chrono::ceil<std::chrono::milliseconds>(
                remaining
            ).count()
        );
    }
}

int main(int argc, char *argv[]) {
    sf_args args;

    if(parse_args(argc, argv, args) < 0) return 1;

    if(!valid_args(args)) {
        std::cerr << "Invalid usage: " << args.error << std::endl;
        return 1;
    }

    if(args.print_usage) {
        usage();
        return 0;
    }

    int startup_packets = (
        args.buffer_delay * args.sample_rate
    ) / (
        1000 * frames
    );

    sf::audio_capture capture;
    sf::audio_sink sink;
    sf::channel<float> capture_pipe(4096);
    sf::server server;
    sf::channel<sf::audio_packet_t> server_pipe(128);
    jitter_buf_t jitter_buf(args.sample_rate, startup_packets);
    std::atomic<bool> shutdown_requested{false};

    uint64_t accepted_count = 0;
    uint64_t duplicate_count = 0;
    uint64_t too_old_count = 0;
    uint64_t too_far_ahead_count = 0;
    uint64_t collision_count = 0;

    uint64_t audio_count = 0;
    uint64_t concealed_count = 0;
    uint64_t buffering_count = 0;
    uint64_t not_due_count = 0;

    uint64_t n_host_sent = 0;
    uint64_t n_host_received = 0;

    std::cout << "Initializing server...\n";
    if(server.init() < 0) {
        std::cerr << "Could not initialize server\n";
        return 1;
    }

    std::cout << "Opening audio sink...\n";
    if(sink.open(args.sample_rate) < 0) {
        std::cerr << "Could not open audio sink\n";
        return 1;
    }

    std::cout << "Opening audio capture...\n";
    if(capture.open(args.sample_rate, capture_pipe) < 0) {
        std::cerr << "Could not open audio capture\n";
        sink.close();
        return 1;
    }

    sigset_t signals;

    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);

    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    // The audio capture thread should capture a packet worth of data and then send
    // it to the remote address (device).
    auto audio_capture_thread = std::thread([&] {
        // Wait for the server to be functioning.
        server.wait();

        sf::audio_packet_t packet{};
        packet.header.seq = 0;
        packet.header.frame_count = frames;
        packet.header.timestamp = 0;
        packet.header.flags = 0x0;

        while(!shutdown_requested.load()) {
            float *packet_data = packet.samples;
            
            // 1. Capture a packet worth of data and fill the packet samples.
            capture_pipe.wait_n(frames, -1, [&packet_data](float *samples, std::size_t n) {
                for(std::size_t i = 0; i < n; i++) {
                    *packet_data++ = samples[i];
                }
            });

            // 2. Send the packet to the remote address.
            if(server.send(args.remote_addr.c_str(), args.remote_port, packet) > 0) {
                n_host_sent++;
            }

            // 3. Update next packet sequence and timestamp.
            packet.header.seq += 1;
            packet.header.timestamp += frames; 
        }
    });

    // The audio playback thread is responsible for playing audio coming back to the server.
    // It needs to meet deadlines and use a jitter buffer to reorganize incoming packets.
    auto audio_playback_thread = std::thread([&] {
        // Wait for the server to start playing audio.
        server.wait();

        jitter_buf_t::packet_t jitter_packet{};
        std::array<float, frames> samples{};

        while(!shutdown_requested.load()) {
            int timeout = timeout_ms(jitter_buf);

            server_pipe.wait(timeout, [&](sf::audio_packet_t *packet) {
                jitter_packet.seq = packet->header.seq;
                jitter_packet.timestamp = packet->header.timestamp;

                if(packet->header.frame_count != frames) {
                    return;
                }

                std::copy(
                    packet->samples, 
                    packet->samples + frames, 
                    jitter_packet.samples.data()
                );

                switch(jitter_buf.insert(jitter_packet)) {
                    case jitter_buf_t::insert_result_t::accepted:
                        accepted_count++;
                        break;

                    case jitter_buf_t::insert_result_t::duplicate:
                        duplicate_count++;
                        break;

                    case jitter_buf_t::insert_result_t::too_old:
                        too_old_count++;
                        break;

                    case jitter_buf_t::insert_result_t::too_far_ahead:
                        too_far_ahead_count++;
                        break;

                    case jitter_buf_t::insert_result_t::slot_collision:
                        collision_count++;
                        break;
                }

                n_host_received++;
            });

            auto result = jitter_buf.playout(
                jitter_buf_t::clock_t::now(),
                samples
            );

            switch(result) {
                case jitter_buf_t::playout_result_t::concealed:
                    concealed_count++;
                    sink.write(samples.data(), samples.size());
                    break;
                case jitter_buf_t::playout_result_t::audio:
                    audio_count++;
                    sink.write(samples.data(), samples.size());
                    break;

                case jitter_buf_t::playout_result_t::buffering:
                    buffering_count++;
                    break;
                case jitter_buf_t::playout_result_t::not_due:
                    not_due_count++;
                    break;
            }
        }

        // std::cout << "Jitter buffer late periods: " << buf.late_periods() << std::endl;
    });

    auto server_thread = std::thread([&] {
        server.listen(args.host_port, server_pipe);
    });

    // Main thread should manage lifecycles
    int signal = 0;
    sigwait(&signals, &signal);
    shutdown_requested.store(true);

    server.stop();
    std::cout << "Server stopped.\n";

    server_pipe.stop();
    capture_pipe.stop();
    std::cout << "Pipes closed.\n";

    audio_capture_thread.join();
    audio_playback_thread.join();
    server_thread.join();
    
    capture.close();
    std::cout << "Audio capture closed.\n";

    sink.close();
    std::cout << "Audio sink closed.\n";

    // std::cout << "Audio capture overflows: " << capture.overflows() << std::endl;
    // std::cout << "Audio sink overflows: " << sink.overflows() << std::endl;
    // std::cout << "Audio sink underflows: " << sink.underflows() << std::endl;
    // std::cout << "Server overflows: " << server.overflows() << std::endl;
    
    // std::cout << "Accepted count: " << accepted_count << std::endl;
    // std::cout << "Duplicate count: " << duplicate_count << std::endl;
    // std::cout << "Too old count: " << too_old_count << std::endl;
    // std::cout << "Too far ahead count: " << too_far_ahead_count << std::endl;
    // std::cout << "Collision count: " << collision_count << std::endl;
    
    // std::cout << "Audio count: " << audio_count << std::endl;
    // std::cout << "Concealed count: " << concealed_count << std::endl;
    // std::cout << "Buffering count: " << buffering_count << std::endl;
    // std::cout << "Not due count: " << not_due_count << std::endl;

    // std::cout << "Host sent: " << n_host_sent << std::endl;
    // std::cout << "Host received: " << n_host_received << std::endl;

    return 0;
}