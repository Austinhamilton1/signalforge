#include <atomic>
#include <cstdint>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "protocol.hpp"
#include "serializer.hpp"
#include "data/channel.hpp"

namespace sf {
    class server {
    public:
        server() = default;

        ~server() {
            if(m_server_fd >= 0) {
                close(m_server_fd);
            }

            if(m_client_fd >= 0) {
                close(m_client_fd);
            }

            if(m_epoll_fd >= 0) {
                close(m_epoll_fd);
            }

            if(m_stop_fd >= 0) {
                close(m_stop_fd);
            }
        }

        /*
         * Setup the server socket and epoll instance.
         * 
         * @return 0 on success, non-zero on failure.
         */
        int init() {
            // Use UDP socket for real-time audio streaming
            // TCP is not suitable for real-time audio due to its retransmission and congestion control mechanisms.
            if((m_server_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
                return -1;
            }

            if((m_client_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
                close(m_server_fd);
                m_server_fd = -1;
                return -1;
            }

            // Ensure the socket is non-blocking for epoll to work correctly
            if(!make_socket_nonblocking(m_server_fd)) {
                close(m_server_fd);
                close(m_client_fd);
                m_server_fd = -1;
                m_client_fd = -1;
                return -1;
            }

            if(!make_socket_nonblocking(m_client_fd)) {
            }

            m_stop_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

            if(m_stop_fd < 0) {
                close(m_server_fd);
                close(m_client_fd);
                m_server_fd = -1;
                m_client_fd = -1;
                return -1;
            }

            std::memset(&m_server_addr, 0, sizeof(m_server_addr));
            std::memset(&m_client_addr, 0, sizeof(m_client_addr));

            m_events.resize(10); // Arbitrary size for the events vector
            m_recv_buffer.resize(detail::max_packet_size);
            m_send_buffer.resize(detail::max_packet_size);

            return 0;
        }

        int send(const char *ip_address, int port, const audio_packet_t& data) {
            /*
             * Set up the socket for asynchronous messaging.
             */
            m_client_addr.sin_family = AF_INET;                     // IPv4
            m_client_addr.sin_port = htons(port);                   // Bind to specified port.
            m_client_addr.sin_addr.s_addr = inet_addr(ip_address);  // Send to specified address.
        
            socklen_t len = sizeof(m_client_addr);

            size_t packet_size = sf::serializer::serialize(m_send_buffer.data(), data, m_send_buffer.size());
            if(packet_size == 0) {
                return -1;
            }

            ssize_t bytes = sendto(
                m_client_fd,
                m_send_buffer.data(),
                packet_size,
                MSG_CONFIRM | MSG_DONTWAIT,
                (const struct sockaddr *)&m_client_addr,
                len
            );

            return bytes;
        }

        /*
         * Start the server's event loop to listen for incoming audio packets.
         *
         * @param port Port to listen on.
         * @param pipe A reference to a channel to output data to.
         */
        void listen(int port, channel<audio_packet_t>& pipe) {
            /*
             * Set up and bind the socket for listening.
             */
            m_server_addr.sin_family = AF_INET;         // IPv4
            m_server_addr.sin_addr.s_addr = INADDR_ANY; // Bind to any incoming address
            m_server_addr.sin_port = htons(port);       // Bind to specified port

            if(bind(m_server_fd, (const struct sockaddr *)&m_server_addr, sizeof(m_server_addr)) < 0) {
                close(m_server_fd);
                close(m_client_fd);
                m_server_fd = -1;
                m_client_fd = -1;
                return;
            }

            if((m_epoll_fd = epoll_create1(0)) < 0) {
                close(m_server_fd);
                close(m_client_fd);
                m_server_fd = -1;
                m_client_fd = -1;
                return;
            }

            struct epoll_event ev;
            ev.events = EPOLLIN | EPOLLET;
            ev.data.fd = m_server_fd;

            if(epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, m_server_fd, &ev) < 0) {
                return;
            }

            struct epoll_event stop_ev;
            stop_ev.events = EPOLLIN;
            stop_ev.data.fd = m_stop_fd;

            if(epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, m_stop_fd, &stop_ev) < 0) {
                return;
            }

            // Start the server
            m_running.store(true, std::memory_order_relaxed);

            // Event loop
            while(m_running.load(std::memory_order_relaxed)) {
                int nfds = epoll_wait(m_epoll_fd, m_events.data(), m_events.size(), -1);
                if(nfds < 0) {
                    if(errno == EINTR) {
                        continue; // Interrupted by signal, retry
                    }
                    break;
                }

                for(int n = 0; n < nfds; n++) {
                    if(m_events[n].data.fd == m_stop_fd) {
                        uint64_t signal;
                        read(m_stop_fd, &signal, sizeof(signal));

                        m_running.store(false, std::memory_order_relaxed);
                        break;
                    }

                    if(m_events[n].data.fd == m_server_fd) {
                        while(true) {
                            socklen_t addr_len = sizeof(m_client_addr);
                            ssize_t recv_len = recvfrom(
                                m_server_fd,
                                m_recv_buffer.data(),
                                m_recv_buffer.size(),
                                0,
                                (struct sockaddr*)&m_server_addr,
                                &addr_len
                            );

                            if(recv_len < 0) {
                                if(errno == EAGAIN || errno == EWOULDBLOCK) {
                                    // No more data to read
                                    break;
                                } else {
                                    // Handle error
                                    break;
                                }
                            }

                            // Process the received packet
                            if(recv_len >= sizeof(sf::audio_packet_header_t)) {
                                sf::audio_packet_t packet;

                                if(sf::serializer::deserialize(packet, m_recv_buffer.data(), static_cast<size_t>(recv_len)) > 0) {
                                    // Insert the packet into the pipe
                                    pipe.publish([packet](sf::audio_packet_t *slot) {
                                        *slot = packet;
                                    });
                                }
                            }
                        }
                    }
                }
            }
        }

        /*
         * Check if the server is running.
         */
        bool running() const noexcept {
            return m_running.load(std::memory_order_relaxed);
        }

        /*
         * Stop the server listen loop.
         */
        void stop() noexcept {
            m_running.store(false, std::memory_order_relaxed);

            uint64_t signal = 1;

            write(m_stop_fd, &signal, sizeof(signal));
        }

        /*
         * Pause the calling thread until the server is up
         * and running.
         */
        void wait() const noexcept {
            while(!running()) {
                std::this_thread::yield();
            }
        }

    private:
        bool make_socket_nonblocking(int fd) {
            int flags = fcntl(fd, F_GETFL, 0);
            if(flags == -1) return false;
            return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
        }

        int m_server_fd = -1;
        int m_client_fd = -1;
        int m_epoll_fd = -1;
        int m_stop_fd = -1;

        struct sockaddr_in m_server_addr;
        struct sockaddr_in m_client_addr;

        std::vector<struct epoll_event> m_events;
        std::vector<uint8_t> m_recv_buffer;
        std::vector<uint8_t> m_send_buffer;

        std::atomic<bool> m_running{false};
    };
}