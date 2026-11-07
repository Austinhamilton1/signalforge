#pragma once

#include <atomic>
#include <thread>

#include <pipewire-0.3/pipewire/pipewire.h>

#include <pipewire-0.3/pipewire/keys.h>
#include <pipewire-0.3/pipewire/main-loop.h>
#include <pipewire-0.3/pipewire/port.h>
#include <pipewire-0.3/pipewire/properties.h>
#include <pipewire-0.3/pipewire/filter.h>

#include <spa-0.2/spa/node/io.h>
#include <spa-0.2/spa/param/latency.h>
#include <spa-0.2/spa/param/latency-utils.h>
#include <spa-0.2/spa/param/param.h>
#include <spa-0.2/spa/pod/builder.h>
#include <spa-0.2/spa/pod/pod.h>
#include <spa-0.2/spa/utils/defs.h>

#include "data/channel.hpp"

namespace sf {
    struct port_t {
        struct pw_filter *filter;
        void *pw_port;
    };

    class audio_capture {
    public:
        audio_capture() {}
        ~audio_capture() {
            close();
        }

        audio_capture(const audio_capture&) = delete;
        audio_capture& operator=(const audio_capture&) = delete;
        audio_capture(audio_capture&&) = delete;
        audio_capture& operator=(audio_capture&&) = delete;

        /*
         * Initialize the audio capture. 
         * 
         * @param sample_rate Sample rate of the capture.
         * @param pipe Output audio to this channel.
         * 
         * @return 0 on success, -1 on failure.
         */
        int open(uint32_t sample_rate, channel<float>& pipe) noexcept {
            if(m_open.load()) return -1;

            if(sample_rate == 0) return -1;

            m_pipe = &pipe;

            const struct spa_pod *params[1];
            uint8_t buffer[1024];
            struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

            pw_init(nullptr, nullptr);

            if((m_loop = pw_main_loop_new(NULL)) == NULL) {
                m_loop = nullptr;
                pw_deinit();
                return -1;
            }

            m_filter_events.version = PW_VERSION_FILTER_EVENTS;
            m_filter_events.process = &audio_capture::on_process;

            m_filter = pw_filter_new_simple(
                pw_main_loop_get_loop(m_loop),
                "signal_forge",
                pw_properties_new(
                    PW_KEY_MEDIA_TYPE, "Audio",
                    PW_KEY_MEDIA_CATEGORY, "Filter",
                    PW_KEY_MEDIA_ROLE, "Music",
                    NULL
                ),
                &m_filter_events,
                this
            );

            if(m_filter == NULL) {
                m_filter = nullptr;
                pw_main_loop_destroy(m_loop);
                m_loop = nullptr;
                pw_deinit();
                return -1;
            }

            m_in_port.pw_port = pw_filter_add_port(
                m_filter,
                PW_DIRECTION_INPUT,
                PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                sizeof(port_t),
                pw_properties_new(
                    PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                    PW_KEY_PORT_NAME, "input",
                    NULL
                ),
                NULL,
                0
            );

            // Set processing latency information
            spa_process_latency_info latency_info{};
            latency_info.ns = 10 * SPA_NSEC_PER_MSEC;
            latency_info.rate = static_cast<int32_t>(sample_rate);

            params[0] = spa_process_latency_build(
                &b, SPA_PARAM_ProcessLatency, &latency_info
            );

            int result = pw_filter_connect(
                m_filter,
                PW_FILTER_FLAG_RT_PROCESS,
                params,
                1
            );

            if(result < 0) {
                pw_filter_destroy(m_filter);
                m_filter = nullptr;
                pw_main_loop_destroy(m_loop);
                m_loop = nullptr;
                pw_deinit();
                return -1;
            }

            m_open.store(true);

            try {
                m_loop_thread = std::thread([this] {
                    pw_main_loop_run(m_loop);
                });
            } catch(...) {
                m_open.store(false);
                pw_filter_disconnect(m_filter);
                pw_filter_destroy(m_filter);
                m_filter = nullptr;
                pw_main_loop_destroy(m_loop);
                m_loop = nullptr;
                pw_deinit();
                return -1;
            }

            return 0;
        }

        /*
         * Close the audio capture. 
         */
        void close() noexcept {
            if(!m_loop) return;

            m_open.store(false);

            pw_main_loop_quit(m_loop);

            if(m_loop_thread.joinable())
                m_loop_thread.join();

            if(m_filter) {
                pw_filter_disconnect(m_filter);
                pw_filter_destroy(m_filter);
                m_filter = nullptr;
            }

            pw_main_loop_destroy(m_loop);
            m_loop = nullptr;

            pw_deinit();
        }

    private:
        static void on_process(void *data, struct spa_io_position *position) {
            auto *self =
                static_cast<sf::audio_capture *>(data);

            self->process(position);
        }

        struct pw_filter_events m_filter_events {};
        
        /*
         * Event processing handling. This function reads data from
         * stream and outputs it to the pipe.
         */
        void process(struct spa_io_position *position) {
            float *in;
            uint32_t n_samples = position->clock.duration;

            in = static_cast<float *>(
                pw_filter_get_dsp_buffer(m_in_port.pw_port, n_samples)
            );

            if(in == nullptr) return;

            // Capture audio
            m_pipe->publish_n(in, n_samples);
        }

        // Buffer to read/write samples from
        channel<float> *m_pipe = nullptr;

        // Pipewire internals
        struct pw_main_loop *m_loop = nullptr;
        struct pw_filter *m_filter = nullptr;
        port_t m_in_port{};

        std::atomic<bool> m_open{false};

        std::thread m_loop_thread;
    };
}