#include <atomic>
#include <csignal>
#include <iostream>
#include <pthread.h>
#include <string>
#include <thread>
#include <unistd.h>

#include "data/channel.hpp"
#include "network/protocol.hpp"
#include "network/server.hpp"

namespace {
    struct sf_args {
        bool invalid_argument = false;
        bool print_usage = false;
        int host_port = 8080;
        int remote_port = 8080;
        std::string remote_addr = "";
        std::string error = "";
    };

    void usage() {
        std::cout << "=====================================================================\n";
        std::cout << "Usage: test_backend [options] [device IP]\n";
        std::cout << "Options:\n";
        std::cout << "    -h                Print usage\n";
        std::cout << "    -s <port>         Specify source port (default 8080)\n";
        std::cout << "    -d <port>         Specify destination port (default 8080)\n";
        std::cout << "=====================================================================\n";
    }

    int parse_args(int argc, char **argv, sf_args& args) {
        int opt;

        while((opt = getopt(argc, argv, "hs:d:")) != -1) {
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
                case '?':
                    args.invalid_argument = true;
                    break;
                default:
                    std::cerr << "Invalid argument: " << opt << std::endl;
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

        if(args.remote_addr.size() == 0) {
            args.error = "Missing remote address";
            return false;
        }

        return true;
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

    sf::server server;
    sf::channel<sf::audio_packet_t> pipe(32);
    std::atomic<bool> shutdown_requested{false};
    uint64_t n_backend_sent = 0;
    uint64_t n_backend_received = 0;

    std::cout << "Initializing server...\n";
    if(server.init() < 0) {
        std::cerr << "Could not initialize server\n";
        return 1;
    }

    sigset_t signals;

    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);

    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    auto copy_audio_thread = std::thread([&] {
        server.wait();

        while(!shutdown_requested.load()) {
            // Send all packets directly back to the sender.
            pipe.wait(-1, [&](sf::audio_packet_t *packet) {
                n_backend_received++;
                if(server.send(args.remote_addr.c_str(), args.remote_port, *packet) > 0) {
                    n_backend_sent++;
                }
            });
        }
    });

    auto server_thread = std::thread([&] {
        server.listen(args.host_port, pipe);
    });

    int signal = 0;
    sigwait(&signals, &signal);
    shutdown_requested.store(true);

    server.stop();
    std::cout << "Server closed.\n";

    pipe.stop();
    std::cout << "Pipes closed.\n";

    copy_audio_thread.join();
    server_thread.join();

    // std::cout << "Server overflows: " << server.overflows() << std::endl;
    // std::cout << "Server received: " << n_backend_received << std::endl;
    // std::cout << "Server sent: " << n_backend_sent << std::endl;

    return 0;
}