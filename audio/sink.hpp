#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <thread>

#include <pipewire-0.3/pipewire/pipewire.h>

#include <pipewire-0.3/pipewire/keys.h>
#include <pipewire-0.3/pipewire/log.h>
#include <pipewire-0.3/pipewire/main-loop.h>
#include <pipewire-0.3/pipewire/port.h>
#include <pipewire-0.3/pipewire/properties.h>
#include <pipewire-0.3/pipewire/stream.h>

#include <spa-0.2/spa/buffer/buffer.h>
#include <spa-0.2/spa/param/audio/raw.h>
#include <spa-0.2/spa/param/audio/raw-utils.h>
#include <spa-0.2/spa/param/param.h>
#include <spa-0.2/spa/pod/builder.h>
#include <spa-0.2/spa/pod/pod.h>
#include <spa-0.2/spa/utils/defs.h>

#include <pipewire-0.3/pipewire/core.h>

#include "data/zero_copy_spsc.hpp"

namespace {
    constexpr uint64_t startup_buf = 960;
}

namespace sf {
    template<std::size_t Frames>
    class audio_sink {
        static_assert(Frames > 0);

    public:
        audio_sink(std::size_t buffer_size = 2048) : m_samples(buffer_size) {}
        ~audio_sink() {
            close();
        }

        audio_sink(const audio_sink&) = delete;
        audio_sink& operator=(const audio_sink&) = delete;
        audio_sink(audio_sink&&) = delete;
        audio_sink& operator=(audio_sink&&) = delete;

        /*
         * Initialize the audio sink. 
         * **MUST** be called before writing to the buffer
         * or the audio sink will not produce samples.
         * 
         * @param sample_rate Sample rate of the sink.
         * 
         * @return 0 on success, -1 on failure.
         */
        int open(uint32_t sample_rate) noexcept {
            if(m_open.load()) return -1;

            if(sample_rate == 0) return -1;

            const struct spa_pod *params[1];
            uint8_t buffer[1024];
            struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

            pw_init(nullptr, nullptr);

            if((m_loop = pw_main_loop_new(NULL)) == NULL) {
                m_loop = nullptr;
                pw_deinit();
                return -1;
            }

            m_stream_events.version = PW_VERSION_STREAM_EVENTS;
            m_stream_events.process = &audio_sink::on_process;

            m_stream = pw_stream_new_simple(
                pw_main_loop_get_loop(m_loop),
                "audio-src",
                pw_properties_new(
                    PW_KEY_MEDIA_TYPE, "Audio",
                    PW_KEY_MEDIA_CATEGORY, "Playback",
                    PW_KEY_MEDIA_ROLE, "Music",
                    NULL
                ),
                &m_stream_events,
                this
            );

            if(m_stream == NULL) {
                m_stream = nullptr;
                pw_main_loop_destroy(m_loop);
                m_loop = nullptr;
                pw_deinit();
                return -1;
            }

            // Request mono, interleaved 32-bit floating-point audio
            spa_audio_info_raw audio_info{};
            audio_info.format = SPA_AUDIO_FORMAT_F32;
            audio_info.channels = 1;
            audio_info.rate = sample_rate;

            params[0] = spa_format_audio_raw_build(
                &b, SPA_PARAM_EnumFormat, &audio_info
            );

            int result = pw_stream_connect(
                m_stream,
                PW_DIRECTION_OUTPUT,
                PW_ID_ANY,
                static_cast<pw_stream_flags>(
                    PW_STREAM_FLAG_AUTOCONNECT |
                    PW_STREAM_FLAG_MAP_BUFFERS |
                    PW_STREAM_FLAG_RT_PROCESS
                ),
                params,
                1
            );

            if(result < 0) {
                pw_stream_destroy(m_stream);
                m_stream = nullptr;
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
                pw_stream_disconnect(m_stream);
                pw_stream_destroy(m_stream);
                m_stream = nullptr;
                pw_main_loop_destroy(m_loop);
                m_loop = nullptr;
                pw_deinit();
                return -1;
            }

            return 0;
        }

        /*
         * Close the audio sink. 
         */
        void close() noexcept {
            if(!m_loop) return;

            m_open.store(false);

            pw_main_loop_quit(m_loop);

            if(m_loop_thread.joinable())
                m_loop_thread.join();

            if(m_stream) {
                pw_stream_disconnect(m_stream);
                pw_stream_destroy(m_stream);
                m_stream = nullptr;
            }

            pw_main_loop_destroy(m_loop);
            m_loop = nullptr;

            pw_deinit();
        }

        /*
         * Write data to the audio sink to be played.
         *
         * @param samples Data to be played by the sink.
         */
        void write(const std::array<float, Frames>& samples) noexcept {
            for(std::size_t i = 0; i < samples.size(); i++) {
                auto *sample = m_samples.write();
                if(!sample) {
                    m_overruns.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }

                *sample = samples[i];
                m_samples.commit();
                m_sample_count.fetch_add(1, std::memory_order_relaxed);
            }
        }

        uint64_t underruns() const noexcept {
            return m_underruns.load(std::memory_order_relaxed);
        }

        uint64_t underrun_frames() const noexcept {
            return m_underrun_frames.load(std::memory_order_relaxed);
        }

        uint64_t overruns() const noexcept {
            return m_overruns.load(std::memory_order_relaxed);
        }

    private:
        static void on_process(void *userdata) {
            auto *self =
                static_cast<sf::audio_sink<Frames> *>(userdata);

            self->process();
        }

        struct pw_stream_events m_stream_events {};
        
        /*
         * Event processing handling. This function reads data from
         * the sink's internal buffer and then writes it to the audio
         * stream.
         */
        void process() {
            struct pw_buffer *b;
            struct spa_buffer *buf;
            int i, n_frames, stride, n_samples;
            float *dst;

            if((b = pw_stream_dequeue_buffer(m_stream)) == NULL) {
                pw_log_warn("out of buffers: %m");
                return;
            }

            buf = b->buffer;
            if((dst = static_cast<float *>(buf->datas[0].data)) == NULL) {
                pw_stream_queue_buffer(m_stream, b);
                return;
            }

            stride = sizeof(float);
            n_frames = buf->datas[0].maxsize / stride;
            if(b->requested) {
                n_frames = SPA_MIN(b->requested, n_frames);
            }

            if(m_sample_count.load(std::memory_order_relaxed) < startup_buf) {
                // Wait until startup_buf samples are available to read.
                std::fill(dst, dst + n_frames, 0.0f);
            } else {
                for(i = 0, n_samples = 0; i < n_frames; i++) {
                    auto *sample = m_samples.read();
                    if(!sample) {
                        continue;
                    }

                    *dst++ = *sample;
                    n_samples++;
                    m_samples.release();
                }

                // Fill missing data with silence
                int missing_samples = n_frames - n_samples;
                if(missing_samples > 0) {
                    m_underruns.fetch_add(1, std::memory_order_relaxed);
                    m_underrun_frames.fetch_add(
                        missing_samples,
                        std::memory_order_relaxed
                    );
                    std::fill(dst, dst + missing_samples, 0.0f);
                }
            }

            buf->datas[0].chunk->offset = 0;
            buf->datas[0].chunk->stride = stride;
            buf->datas[0].chunk->size = n_frames * stride;

            pw_stream_queue_buffer(m_stream, b);
        }

        // Buffer to read/write samples from
        zc_spsc<float> m_samples;

        // Pipewire internals
        struct pw_main_loop *m_loop = nullptr;
        struct pw_stream *m_stream = nullptr;

        std::atomic<bool> m_open{false};

        std::atomic<uint64_t> m_sample_count{0};

        std::atomic<uint64_t> m_underruns{0};
        std::atomic<uint64_t> m_overruns{0};
        std::atomic<uint64_t> m_underrun_frames{0};

        std::thread m_loop_thread;
    };
}