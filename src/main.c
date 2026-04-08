// SPDX-License-Identifier: MIT
// Turret Plugin - Orientation-triggered sound player
//
// Detects when the device is put down (standby) or picked up (active)
// using gyroscope and accelerometer data, and plays random turret sounds.

#include "tanmatsu_plugin.h"
#include "asp/orientation.h"
#include "audio.h"
#include <string.h>
#include <stdio.h>
#include <dirent.h>

#define TAG "turrent"
#define SOUND_DIR "/sd/turret"
#define MAX_SOUNDS 16
#define LED_INDEX 4

// Schmitt trigger states — only two states, no idle
typedef enum {
    STATE_STANDBY,  // Device is lying down
    STATE_ACTIVE,   // Device is picked up / held
} turret_state_t;

// Schmitt trigger thresholds on tilt² = ax² + ay² (squared to avoid sqrtf)
// tilt² ~0 when flat, ~48 at 45°, ~96 when vertical
#define TILT2_PICKUP_THRESHOLD  25.0f  // 5.0² — must exceed to transition STANDBY → ACTIVE
#define TILT2_PUTDOWN_THRESHOLD  4.0f  // 2.0² — must go below to transition ACTIVE → STANDBY
#define GYRO2_CALM_THRESHOLD     2.25f // 1.5² — max gyro magnitude² to count as "still"
#define PICKUP_HOLD_MS          1500   // How long tilt must exceed threshold to trigger pickup
#define PUTDOWN_HOLD_MS         3000   // How long device must be flat+still to trigger putdown

// Simple PRNG (rand/srand not available in kbelf libc)
static uint32_t prng_state = 1;

static void prng_seed(uint32_t seed) {
    prng_state = seed ? seed : 1;
}

static uint32_t prng_next(void) {
    prng_state ^= prng_state << 13;
    prng_state ^= prng_state >> 17;
    prng_state ^= prng_state << 5;
    return prng_state;
}

// Sound file lists
static char startup_files[MAX_SOUNDS][280];
static int startup_count = 0;
static char standby_files[MAX_SOUNDS][280];
static int standby_count = 0;

// Scan sound directory for startup_*.mp3 and standby_*.mp3
static void scan_sound_files(void) {
    DIR* dir = opendir(SOUND_DIR);
    if (!dir) {
        asp_log_error(TAG, "Failed to open sound dir: %s", SOUND_DIR);
        return;
    }

    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        const char* name = ent->d_name;
        size_t len = strlen(name);

        // Must end with .mp3
        if (len < 5 || strcmp(name + len - 4, ".mp3") != 0) {
            continue;
        }

        if (strncmp(name, "startup_", 8) == 0 && startup_count < MAX_SOUNDS) {
            snprintf(startup_files[startup_count], sizeof(startup_files[0]),
                     "%s/%s", SOUND_DIR, name);
            startup_count++;
        } else if (strncmp(name, "standby_", 8) == 0 && standby_count < MAX_SOUNDS) {
            snprintf(standby_files[standby_count], sizeof(standby_files[0]),
                     "%s/%s", SOUND_DIR, name);
            standby_count++;
        }
    }

    closedir(dir);
    asp_log_info(TAG, "Found %d startup, %d standby sounds", startup_count, standby_count);
}

// Play a random sound from the given list
static void play_random_sound(char files[][280], int count) {
    if (count <= 0) return;
    int idx = prng_next() % count;
    asp_log_info(TAG, "Playing: %s", files[idx]);
    audio_play_file(files[idx]);
}

// Plugin metadata
static const plugin_info_t plugin_info = {
    .name = "Turret",
    .slug = "turrent",
    .version = "1.0.0",
    .author = "Tanmatsu",
    .description = "Plays turret sounds when device is picked up or put down",
    .api_version = TANMATSU_PLUGIN_API_VERSION,
    .type = PLUGIN_TYPE_SERVICE,
    .flags = 0,
};

static const plugin_info_t* get_info(void) {
    return &plugin_info;
}

static plugin_context_t* plugin_ctx = NULL;

static int plugin_init(plugin_context_t* ctx) {
    asp_log_info(TAG, "Turret plugin initializing...");
    plugin_ctx = ctx;

    // Claim LED
    if (!asp_plugin_led_claim(ctx, LED_INDEX)) {
        asp_log_warn(TAG, "Failed to claim LED %d", LED_INDEX);
    }

    // Start with LED off
    asp_led_set_pixel_rgb(LED_INDEX, 0, 0, 0);
    asp_led_send();

    return 0;
}

static void plugin_cleanup(plugin_context_t* ctx) {
    asp_log_info(TAG, "Turret plugin cleaning up...");

    // Turn off LED and release
    asp_led_set_pixel_rgb(LED_INDEX, 0, 0, 0);
    asp_led_send();
    asp_plugin_led_release(ctx, LED_INDEX);

    plugin_ctx = NULL;
}

static void plugin_service_run(plugin_context_t* ctx) {
    asp_log_info(TAG, "Turret service starting...");

    // Initialize audio
    if (audio_init() != 0) {
        asp_log_error(TAG, "Audio init failed");
        return;
    }

    // Enable sensors
    asp_orientation_enable_gyroscope();
    asp_orientation_enable_accelerometer();

    // Scan sound files
    scan_sound_files();

    // Seed PRNG
    prng_seed(asp_plugin_get_tick_ms());

    // Read initial orientation to set starting state (no sound on init)
    bool gyro_ready = false, accel_ready = false;
    float gx = 0, gy = 0, gz = 0;
    float ax = 0, ay = 0, az = 0;
    turret_state_t state;

    asp_orientation_get(&gyro_ready, &accel_ready, &gx, &gy, &gz, &ax, &ay, &az);

    float tilt2 = ax * ax + ay * ay;
    if (tilt2 > TILT2_PICKUP_THRESHOLD) {
        state = STATE_ACTIVE;
        asp_led_set_pixel_rgb(LED_INDEX, 255, 0, 0);
        asp_led_send();
        asp_log_info(TAG, "Initial state: ACTIVE (tilt2=%.1f)", tilt2);
    } else {
        state = STATE_STANDBY;
        asp_led_set_pixel_rgb(LED_INDEX, 0, 0, 0);
        asp_led_send();
        asp_log_info(TAG, "Initial state: STANDBY (tilt2=%.1f)", tilt2);
    }

    uint32_t transition_start = 0;
    bool transition_timing = false;
    bool currently_playing = false;

    while (!asp_plugin_should_stop(ctx)) {
        asp_orientation_get(&gyro_ready, &accel_ready, &gx, &gy, &gz, &ax, &ay, &az);

        // If currently playing, wait for it to finish
        if (currently_playing) {
            if (!audio_is_finished()) {
                asp_plugin_delay_ms(50);
                continue;
            }
            audio_stop();
            currently_playing = false;
        }

        uint32_t now = asp_plugin_get_tick_ms();
        tilt2 = ax * ax + ay * ay;
        float gyro2 = gx * gx + gy * gy + gz * gz;

        if (state == STATE_STANDBY) {
            // Looking for pickup: tilt² must exceed high threshold
            if (tilt2 > TILT2_PICKUP_THRESHOLD) {
                if (!transition_timing) {
                    transition_start = now;
                    transition_timing = true;
                }
                if (now - transition_start >= PICKUP_HOLD_MS) {
                    state = STATE_ACTIVE;
                    transition_timing = false;
                    asp_log_info(TAG, "State -> ACTIVE (tilt2=%.1f)", tilt2);
                    asp_led_set_pixel_rgb(LED_INDEX, 255, 0, 0);
                    asp_led_send();
                    play_random_sound(startup_files, startup_count);
                    currently_playing = true;
                }
            } else {
                transition_timing = false;
            }
        } else {
            // STATE_ACTIVE — looking for putdown: tilt² below low threshold AND gyro calm
            if (tilt2 < TILT2_PUTDOWN_THRESHOLD && gyro2 < GYRO2_CALM_THRESHOLD) {
                if (!transition_timing) {
                    transition_start = now;
                    transition_timing = true;
                }
                if (now - transition_start >= PUTDOWN_HOLD_MS) {
                    state = STATE_STANDBY;
                    transition_timing = false;
                    asp_log_info(TAG, "State -> STANDBY (tilt2=%.1f gyro2=%.1f)", tilt2, gyro2);
                    asp_led_set_pixel_rgb(LED_INDEX, 0, 0, 0);
                    asp_led_send();
                    play_random_sound(standby_files, standby_count);
                    currently_playing = true;
                }
            } else {
                transition_timing = false;
            }
        }

        asp_plugin_delay_ms(50);
    }

    // Cleanup audio (leave sensors enabled for other plugins)
    audio_stop();
    audio_cleanup();

    asp_log_info(TAG, "Turret service stopped");
}

// Plugin entry point structure
static const plugin_entry_t entry = {
    .get_info = get_info,
    .init = plugin_init,
    .cleanup = plugin_cleanup,
    .menu_render = NULL,
    .menu_select = NULL,
    .service_run = plugin_service_run,
    .hook_event = NULL,
};

// Register this plugin with the host
TANMATSU_PLUGIN_REGISTER(entry);
