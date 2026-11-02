#include <cstdint>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "protocol.hpp"
#include "serializer.hpp"
#include "data/zero_copy_spsc.hpp"

namespace sf {
    class server {
        using buffer_t = sf::zc_spsc<sf::audio_packet_t>;
    public:
        server() = default;

        ~server() {
            if(m_socket_fd >= 0) {
                close(m_socket_fd);
            }

            if(m_epoll_fd >= 0) {
                close(m_epoll_fd);
            }
        }

        /*
         * Setup the server socket and epoll instance.
         *
         * @param port The port number to bind the server socket to. Default is 8080.
         * 
         * @return 0 on success, non-zero on failure.
         */
        int setup(int port = 8080) {
            // Use UDP socket for real-time audio streaming
            // TCP is not suitable for real-time audio due to its retransmission and congestion control mechanisms.
            if((m_socket_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
                return -1;
            }

            std::memset(&m_server_addr, 0, sizeof(m_server_addr));
            std::memset(&m_client_addr, 0, sizeof(m_client_addr));

            m_server_addr.sin_family = AF_INET;
            m_server_addr.sin_addr.s_addr = INADDR_ANY;
            m_server_addr.sin_port = htons(port);

            if(bind(m_socket_fd, (const struct sockaddr*)&m_server_addr, sizeof(m_server_addr)) < 0) {
                close(m_socket_fd);
                m_socket_fd = -1;
                return -1;
            }

            // Ensure the socket is non-blocking for epoll to work correctly
            if(!make_socket_nonblocking(m_socket_fd)) {
                close(m_socket_fd);
                m_socket_fd = -1;
                return -1;
            }

            if((m_epoll_fd = epoll_create1(0)) < 0) {
                close(m_socket_fd);
                m_socket_fd = -1;
                m_epoll_fd = -1;
                return -1;
            }

            struct epoll_event ev;
            ev.events = EPOLLIN | EPOLLET;
            ev.data.fd = m_socket_fd;

            if(epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, m_socket_fd, &ev) < 0) {
                close(m_socket_fd);
                close(m_epoll_fd);
                m_socket_fd = -1;
                return -1;
            }

            m_events.resize(10); // Arbitrary size for the events vector
            m_recv_buffer.resize(detail::max_packet_size);

            return 0;
        }

        /*
         * Start the server's event loop to listen for incoming audio packets.
         *
         * @param pipe_event_fd The file descriptor for the event notification pipe.
         * @param pipe A shared pointer to the zero-copy single-producer single-consumer buffer to write the received audio packets into.
         */
        void listen(int pipe_event_fd, std::shared_ptr<buffer_t>& pipe) {
            // Event loop
            while(true) {
                int nfds = epoll_wait(m_epoll_fd, m_events.data(), m_events.size(), -1);
                if(nfds < 0) {
                    if(errno == EINTR) {
                        continue; // Interrupted by signal, retry
                    }
                    break;
                }

                for(int n = 0; n < nfds; n++) {
                    if(m_events[n].data.fd == m_socket_fd) {
                        while(true) {
                            socklen_t addr_len = sizeof(m_client_addr);
                            ssize_t recv_len = recvfrom(
                                m_socket_fd,
                                m_recv_buffer.data(),
                                m_recv_buffer.size(),
                                0,
                                (struct sockaddr*)&m_client_addr,
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

                                if(sf::serializer::deserialize(packet, m_recv_buffer.data(), m_recv_buffer.size()) == 0) {
                                    // Successfully deserialized the packet
                                    // Insert the packet into the pipe
                                    if(pipe) {
                                        if(auto *slot = pipe->write()) {
                                            *slot = packet;
                                            if(pipe->commit()) {
                                                // Signal the event consumer that a new packet is available
                                                uint64_t signal = 1;

                                                ssize_t result = write(
                                                    pipe_event_fd,
                                                    &signal,
                                                    sizeof(signal)
                                                );
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

    private:
        bool make_socket_nonblocking(int fd) {
            int flags = fcntl(fd, F_GETFL, 0);
            if(flags == -1) return false;
            return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
        }

        int m_socket_fd = -1;
        int m_epoll_fd = -1;
        std::vector<struct epoll_event> m_events;
        std::vector<uint8_t> m_recv_buffer;
        struct sockaddr_in m_server_addr;
        struct sockaddr_in m_client_addr;
    };
}