// SPDX-License-Identifier: MIT
// Startup Plugin - Audio Playback
// Simplified from musicplayer - single file playback only

#include "audio.h"
#include "tanmatsu_plugin.h"
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>

// Include minimp3 implementation
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3.h"

// ASP audio API
#include <asp/audio.h>

// ASP file API - asp_fastopen() gives us a DMA-capable internal-RAM stdio
// buffer for /sd and /int paths so the SD-card driver can DMA directly
// into it. Falls back to plain fopen when CONFIG_FATFS_USE_FASTOPEN is
// off in the launcher build.
#include <asp/file.h>

// Buffer sizes
#define READ_BUFFER_SIZE    (16 * 1024)  // 16KB read buffer (PSRAM)
#define REFILL_THRESHOLD    (4 * 1024)   // Only refill when below this watermark
                                         // so SD reads happen in big chunks.
#define MAX_FRAME_SIZE      (1152 * 2)   // Max samples per MP3 frame (stereo)
#define PCM_BUFFER_SIZE     (MAX_FRAME_SIZE * sizeof(int16_t))  // 4608 bytes

// Decoder thread stack size - minimp3 needs >16KB of stack
#define DECODER_STACK_SIZE  (32 * 1024)

// Audio state
static mp3dec_t* g_mp3_decoder = NULL;
static FILE* g_current_file = NULL;
static volatile bool g_playing = false;
static volatile bool g_finished = false;
static volatile uint32_t g_sample_rate = 44100;
static bool g_audio_initialized = false;

// PCM buffer in internal SRAM for DMA (16-byte aligned)
static int16_t pcm_buffer[MAX_FRAME_SIZE] __attribute__((aligned(16)));

// Heap-allocated buffers (PSRAM)
static uint8_t* read_buffer = NULL;
static size_t buffer_pos = 0;
static size_t buffer_len = 0;

// Decoder thread
static pthread_t decoder_thread;
static volatile bool g_thread_running = false;
static volatile bool g_thread_should_stop = false;
static volatile bool g_thread_in_decode = false;

// Path for decoder thread to play
static char g_pending_path[256];
static volatile bool g_new_file_pending = false;

// Fill read buffer from file. Only does an SD read when the unread tail of
// the buffer drops below REFILL_THRESHOLD; otherwise returns the current
// fill level cheaply. Batches FATFS/SD work into infrequent large reads,
// which has far lower latency variance than many small reads.
static size_t fill_buffer(void) {
    if (!g_current_file) {
        return 0;
    }

    size_t available = (buffer_len > buffer_pos) ? (buffer_len - buffer_pos) : 0;
    if (available >= REFILL_THRESHOLD) {
        return available;
    }

    // Move remaining data to start of buffer
    if (buffer_pos > 0 && buffer_len > buffer_pos) {
        memmove(read_buffer, read_buffer + buffer_pos, buffer_len - buffer_pos);
        buffer_len -= buffer_pos;
        buffer_pos = 0;
    } else if (buffer_pos > 0) {
        buffer_len = 0;
        buffer_pos = 0;
    }

    // Read more data - one large chunk to fill the buffer back up
    size_t space = READ_BUFFER_SIZE - buffer_len;
    if (space > 0) {
        size_t bytes_read = fread(read_buffer + buffer_len, 1, space, g_current_file);
        buffer_len += bytes_read;
    }

    return buffer_len - buffer_pos;
}

// Track if we've logged format for current file
static bool g_format_logged = false;

// MP3 decode loop
static void decode_loop(void) {
    mp3dec_frame_info_t info;
    int samples;

    g_thread_in_decode = true;
    while (g_playing && !g_thread_should_stop) {
        size_t available = fill_buffer();

        if (available < 4) {
            // End of file - pause our mixer slot so other plugins regain
            // full volume immediately instead of waiting for our buffer to
            // drain naturally.
            asp_audio_stop();
            g_finished = true;
            g_playing = false;
            asp_log_info("turrent", "Audio playback finished");
            break;
        }

        // Decode one frame
        samples = mp3dec_decode_frame(g_mp3_decoder,
                                       read_buffer + buffer_pos,
                                       buffer_len - buffer_pos,
                                       pcm_buffer, &info);

        if (info.frame_bytes > 0) {
            buffer_pos += info.frame_bytes;
        }

        if (samples > 0) {
            // Log format on first successful decode
            if (!g_format_logged) {
                asp_log_info("turrent", "Format: %d Hz, %d ch, %d kbps",
                            info.hz, info.channels, info.bitrate_kbps);
                g_sample_rate = info.hz;
                g_format_logged = true;
            }

            // Write to audio output
            size_t bytes = samples * info.channels * sizeof(int16_t);
            asp_audio_write(pcm_buffer, bytes, 500);
        } else if (info.frame_bytes == 0) {
            // Need more data or invalid frame
            if (fill_buffer() == 0) {
                asp_audio_stop();
                g_finished = true;
                g_playing = false;
                asp_log_info("turrent", "Audio playback finished (no more data)");
                break;
            }
        }
    }
    g_thread_in_decode = false;
}

// Start playing a new file (called from decoder thread)
static void start_new_file(const char* path) {
    // Close any existing file
    if (g_current_file) {
        asp_fastclose(g_current_file);
        g_current_file = NULL;
    }

    // Open new file
    g_current_file = asp_fastopen(path, "rb");
    if (!g_current_file) {
        asp_log_error("turrent", "Failed to open: %s", path);
        g_playing = false;
        g_finished = true;
        return;
    }

    // Reset decoder state
    mp3dec_init(g_mp3_decoder);
    buffer_pos = 0;
    buffer_len = 0;
    g_finished = false;
    g_format_logged = false;
    g_playing = true;

    // Configure audio (mixer is fixed at 44.1 kHz; no rate change needed).
    // The launcher manages amplifier and master volume.
    asp_audio_start();

    asp_log_info("turrent", "Playing: %s", path);
}

// Decoder thread main function
static void* decoder_thread_func(void* arg) {
    (void)arg;
    asp_log_info("turrent", "Decoder thread started");

    while (!g_thread_should_stop) {
        if (g_new_file_pending) {
            g_new_file_pending = false;
            start_new_file(g_pending_path);
        }

        if (g_playing) {
            decode_loop();
        } else {
            asp_plugin_delay_ms(20);
        }
    }

    asp_log_info("turrent", "Decoder thread exiting");
    g_thread_running = false;
    return NULL;
}

int audio_init(void) {
    if (g_audio_initialized) {
        asp_log_warn("turrent", "Audio already initialized");
        audio_cleanup();
    }

    asp_log_info("turrent", "Allocating audio buffers...");

    read_buffer = (uint8_t*)malloc(READ_BUFFER_SIZE);
    if (!read_buffer) {
        asp_log_error("turrent", "Failed to allocate read_buffer");
        return -1;
    }

    g_mp3_decoder = (mp3dec_t*)malloc(sizeof(mp3dec_t));
    if (!g_mp3_decoder) {
        asp_log_error("turrent", "Failed to allocate mp3_decoder");
        free(read_buffer);
        read_buffer = NULL;
        return -1;
    }

    mp3dec_init(g_mp3_decoder);

    // Create decoder thread with larger stack
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, DECODER_STACK_SIZE);

    g_thread_should_stop = false;
    g_thread_in_decode = false;
    int err = pthread_create(&decoder_thread, &attr, decoder_thread_func, NULL);
    pthread_attr_destroy(&attr);

    if (err != 0) {
        asp_log_error("turrent", "Failed to create decoder thread: %d", err);
        free(read_buffer);
        free(g_mp3_decoder);
        read_buffer = NULL;
        g_mp3_decoder = NULL;
        return -1;
    }

    // Raise the decoder above the plugin task (5) so it preempts UI/widget
    // work right after an SD read returns. The launcher's audio mixer (when
    // enabled) runs at 7, so 6 is safe.
    extern int pthread_setschedprio(pthread_t thread, int prio);
    int prio_err = pthread_setschedprio(decoder_thread, 6);
    if (prio_err != 0) {
        asp_log_warn("turrent", "Failed to raise decoder priority: %d", prio_err);
    }

    g_thread_running = true;
    g_audio_initialized = true;

    asp_log_info("turrent", "Audio initialized");
    return 0;
}

void audio_cleanup(void) {
    if (!g_audio_initialized) {
        return;
    }

    asp_log_info("turrent", "Audio cleanup starting...");

    g_playing = false;
    g_new_file_pending = false;

    // Wait for thread to exit decode_loop
    for (int i = 0; i < 30 && g_thread_in_decode; i++) {
        asp_plugin_delay_ms(20);
    }

    g_thread_should_stop = true;

    if (g_thread_running) {
        for (int i = 0; i < 100 && g_thread_running; i++) {
            asp_plugin_delay_ms(20);
        }
        pthread_join(decoder_thread, NULL);
    }
    g_thread_running = false;

    asp_plugin_delay_ms(50);

    if (g_current_file) {
        asp_fastclose(g_current_file);
        g_current_file = NULL;
    }

    if (read_buffer) {
        free(read_buffer);
        read_buffer = NULL;
    }
    if (g_mp3_decoder) {
        free(g_mp3_decoder);
        g_mp3_decoder = NULL;
    }

    // Reset state
    g_playing = false;
    g_finished = false;
    g_sample_rate = 44100;
    g_format_logged = false;
    g_thread_in_decode = false;
    buffer_pos = 0;
    buffer_len = 0;
    g_new_file_pending = false;
    g_thread_should_stop = false;
    memset(g_pending_path, 0, sizeof(g_pending_path));

    g_audio_initialized = false;
    asp_log_info("turrent", "Audio cleanup complete");
}

void audio_play_file(const char* path) {
    g_playing = false;
    asp_plugin_delay_ms(30);

    strncpy(g_pending_path, path, sizeof(g_pending_path) - 1);
    g_pending_path[sizeof(g_pending_path) - 1] = '\0';
    g_new_file_pending = true;
}

void audio_stop(void) {
    g_playing = false;
    g_new_file_pending = false;
}

bool audio_is_finished(void) {
    return g_finished;
}
