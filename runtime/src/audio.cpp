// Audio output through SDL. Adapted from Zelda64Recomp's main.cpp; the volume is the Sound tab's main volume
// (RecompFrontend, config.cpp). The game's audio tasks run the recompiled aspMain (rsp\aspMain.cpp); the runtime then
// hands us the resulting 16-bit stereo samples through these callbacks.

// SDL includes <Windows.h>; keep its min/max macros from breaking std::min/std::max.
#define NOMINMAX

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define SDL_MAIN_HANDLED
#include "SDL.h"

#include "ultramodern/ultramodern.hpp"
#include "recompui/config.h"

namespace {
    SDL_AudioCVT audio_convert;
    SDL_AudioDeviceID audio_device = 0;

    // Samples per channel per second: the game's rate (set_frequency) and the device's.
    uint32_t sample_rate = 48000;
    uint32_t output_sample_rate = 48000;
    constexpr uint32_t input_channels = 2;
    constexpr uint32_t output_channels = 2;

    // A frame is one sample per channel (unrelated to graphics frames).
    // Frames repeated from the previous chunk so resampling interpolates smoothly across chunk borders.
    constexpr uint32_t duplicated_input_frames = 4;
    // Output frames to skip so the duplicated frames aren't played twice.
    uint32_t discarded_output_frames;

    constexpr uint32_t bytes_per_frame = input_channels * sizeof(float);

    // Debug: if ADAM_AUDIO_DUMP names a file, the game's samples are also written there as raw 16-bit
    // stereo PCM (left first) at the game's sample rate (printed as "[audio] game sample rate").
    FILE* dump_file = nullptr;

    // Size of the game's last audio buffer, in frames (552 at 22050 Hz).
    std::atomic<size_t> last_chunk_frames = 0;

    // Frames still waiting in the SDL queue, in the game's sample rate.
    size_t queued_input_frames() {
        uint64_t output_frames = SDL_GetQueuedAudioSize(audio_device) / (output_channels * sizeof(float));
        return (size_t)(output_frames * sample_rate / output_sample_rate);
    }

    void update_audio_converter() {
        if (SDL_BuildAudioCVT(&audio_convert, AUDIO_F32, input_channels, sample_rate, AUDIO_F32, output_channels, output_sample_rate) < 0) {
            fprintf(stderr, "[audio] SDL converter error: %s\n", SDL_GetError());
            std::exit(EXIT_FAILURE);
        }
        discarded_output_frames = duplicated_input_frames * output_sample_rate / sample_rate;
    }

    void queue_samples(int16_t* audio_data, size_t sample_count) {
        static std::vector<float> swap_buffer;
        static std::array<float, duplicated_input_frames * input_channels> duplicated_sample_buffer;

        if (audio_device == 0 || sample_count <= duplicated_input_frames * input_channels) {
            return;
        }
        // Fast-forward (main.cpp): the game makes audio faster than it can play; mute instead of falling behind.
        if (ultramodern::get_speed_multiplier() > 1) {
            return;
        }

        size_t resampled_sample_count = sample_count + duplicated_input_frames * input_channels;
        size_t max_sample_count = std::max(resampled_sample_count, resampled_sample_count * audio_convert.len_mult);
        if (max_sample_count > swap_buffer.size()) {
            swap_buffer.resize(max_sample_count);
        }

        for (size_t i = 0; i < duplicated_input_frames * input_channels; i++) {
            swap_buffer[i] = duplicated_sample_buffer[i];
        }

        // 16-bit -> float, and swap the channels: RDRAM's byteswapped layout reverses each sample pair.
        const float scale = (0.5f / 32768.0f) * float(recompui::config::sound::get_main_volume() / 100.0);
        for (size_t i = 0; i < sample_count; i += input_channels) {
            swap_buffer[i + 0 + duplicated_input_frames * input_channels] = audio_data[i + 1] * scale;
            swap_buffer[i + 1 + duplicated_input_frames * input_channels] = audio_data[i + 0] * scale;
        }

        for (size_t i = 0; i < duplicated_input_frames * input_channels; i++) {
            duplicated_sample_buffer[i] = swap_buffer[i + sample_count];
        }

        if (dump_file != nullptr) {
            for (size_t i = 0; i < sample_count; i += input_channels) {
                int16_t frame[2] = { audio_data[i + 1], audio_data[i + 0] };
                fwrite(frame, sizeof(frame), 1, dump_file);
            }
            // Also report the production rate: frames received per 5 s of real time vs. the game's rate.
            static uint64_t window_start = SDL_GetTicks64();
            static uint64_t window_frames = 0;
            static uint64_t window_chunks = 0;
            static size_t min_chunk = SIZE_MAX, max_chunk = 0;
            static uint64_t underruns = 0; // chunks that arrived to an empty queue (audible gap), after warm-up
            static uint64_t total_chunks = 0;
            if (++total_chunks > 40 && SDL_GetQueuedAudioSize(audio_device) == 0) {
                underruns++;
            }
            size_t chunk = sample_count / input_channels;
            window_frames += chunk;
            window_chunks++;
            min_chunk = std::min(min_chunk, chunk);
            max_chunk = std::max(max_chunk, chunk);
            uint64_t now = SDL_GetTicks64();
            if (now - window_start >= 5000) {
                double secs = (now - window_start) / 1000.0;
                printf("[audio] %.0f frames/s received (game rate %u): %.1f chunks/s of %zu-%zu frames, %u bytes queued in SDL, %llu underruns so far\n",
                    window_frames / secs, sample_rate, window_chunks / secs, min_chunk, max_chunk, SDL_GetQueuedAudioSize(audio_device),
                    (unsigned long long)underruns);
                window_start = now;
                window_frames = 0;
                window_chunks = 0;
                min_chunk = SIZE_MAX;
                max_chunk = 0;
            }
        }

        audio_convert.buf = reinterpret_cast<Uint8*>(swap_buffer.data());
        audio_convert.len = (int)((sample_count + duplicated_input_frames * input_channels) * sizeof(float));
        if (SDL_ConvertAudio(&audio_convert) < 0) {
            fprintf(stderr, "[audio] SDL convert error: %s\n", SDL_GetError());
            return;
        }

        last_chunk_frames = sample_count / input_channels;

        // The SDL queue holds converted audio: output-rate float frames (Zelda's code divided by the game's rate).
        uint64_t cur_queued_microseconds = uint64_t(SDL_GetQueuedAudioSize(audio_device)) / (output_channels * sizeof(float)) * 1000000 / output_sample_rate;
        uint32_t num_bytes_to_queue = audio_convert.len_cvt - output_channels * discarded_output_frames * sizeof(float);
        float* samples_to_queue = swap_buffer.data() + output_channels * discarded_output_frames / 2;

        // Keep latency down: when more than 100 ms is queued, skip incoming samples (more the further behind).
        uint32_t skip_factor = (uint32_t)(cur_queued_microseconds / 100000);
        if (skip_factor != 0) {
            uint32_t skip_ratio = 1u << std::min(skip_factor, 8u);
            num_bytes_to_queue /= skip_ratio;
            for (size_t i = 0; i < num_bytes_to_queue / (output_channels * sizeof(float)); i++) {
                samples_to_queue[2 * i + 0] = samples_to_queue[2 * skip_ratio * i + 0];
                samples_to_queue[2 * i + 1] = samples_to_queue[2 * skip_ratio * i + 1];
            }
        }

        SDL_QueueAudio(audio_device, samples_to_queue, num_bytes_to_queue);
    }

    size_t get_frames_remaining() {
        if (audio_device == 0) {
            return 0;
        }
        uint64_t buffered_byte_count = SDL_GetQueuedAudioSize(audio_device);
        buffered_byte_count = buffered_byte_count * 2 * sample_rate / output_sample_rate / output_channels;

        // Report one VI's worth less than is queued, so games that size their next chunk from this
        // number generate enough samples even if the audio thread lags a little (prevents popping).
        uint32_t frames_per_vi = sample_rate / 60;
        if (buffered_byte_count > bytes_per_frame * frames_per_vi) {
            buffered_byte_count -= bytes_per_frame * frames_per_vi;
        }
        else {
            buffered_byte_count = 0;
        }
        return (size_t)(buffered_byte_count / bytes_per_frame);
    }

    void set_frequency(uint32_t freq) {
        printf("[audio] game sample rate %u Hz\n", freq);
        sample_rate = freq;
        update_audio_converter();
    }
}

// Called from a hook in the game's audio thread (scripts\extra_hooks.txt) in place of osAiGetStatus's result.
// The real AI has a 2-buffer DMA FIFO and the game waits while it's full; that wait is what paces it to the
// playback rate. Treat "the host queue still holds three whole buffers" as full: more than the hardware
// equivalent, as headroom for PC scheduling hiccups (the game only checks once per VI, and while it waits the
// audio thread runs below the game's frame threads, see extra_hooks.txt; with two buffers SDL ran dry ~0.6x/s).
extern "C" uint32_t adam_ai_status(void) {
    constexpr uint32_t AI_STATUS_FIFO_FULL = 0x80000000;
    constexpr size_t buffers_when_full = 3;
    size_t chunk = last_chunk_frames.load();
    // During fast-forward the audio is dropped, so don't let playback pace (= slow down) the game.
    if (audio_device == 0 || chunk == 0 || ultramodern::get_speed_multiplier() > 1) {
        return 0;
    }
    return queued_input_frames() >= buffers_when_full * chunk ? AI_STATUS_FIFO_FULL : 0;
}

namespace adam::audio {
    // Opens the default output device. Call once before the game starts.
    void init(uint32_t output_freq) {
        if (const char* dump_path = std::getenv("ADAM_AUDIO_DUMP")) {
            dump_file = std::fopen(dump_path, "wb");
            printf("[audio] dumping samples to %s\n", dump_path);
        }

        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            fprintf(stderr, "[audio] SDL audio init failed: %s (continuing without sound)\n", SDL_GetError());
            return;
        }
        SDL_AudioSpec spec_desired{};
        spec_desired.freq = (int)output_freq;
        spec_desired.format = AUDIO_F32;
        spec_desired.channels = (Uint8)output_channels;
        spec_desired.samples = 0x100; // small device buffer = low latency

        audio_device = SDL_OpenAudioDevice(nullptr, false, &spec_desired, nullptr, 0);
        if (audio_device == 0) {
            fprintf(stderr, "[audio] no audio device: %s (continuing without sound)\n", SDL_GetError());
            return;
        }
        SDL_PauseAudioDevice(audio_device, 0);
        output_sample_rate = output_freq;
        update_audio_converter();
    }

    ultramodern::audio_callbacks_t callbacks() {
        return ultramodern::audio_callbacks_t{
            .queue_samples = queue_samples,
            .get_frames_remaining = get_frames_remaining,
            .set_frequency = set_frequency,
        };
    }
}
