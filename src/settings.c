#include "settings.h"
#include "defines.h"
#include "playlist_m3u.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SETTINGS_FILE SHARED_USERDATA_PATH "/music-player/settings.cfg"
#define OVERRIDES_FILE SHARED_USERDATA_PATH "/music-player/overrides.cfg"
#define SETTINGS_DIR SHARED_USERDATA_PATH "/music-player"

static const int screen_off_values[] = {60, 90, 120, 0};
#define SCREEN_OFF_VALUE_COUNT 4
#define DEFAULT_SCREEN_OFF_INDEX 0

static const int bass_filter_values[] = {0, 80, 100, 120, 150, 200};
#define BASS_FILTER_VALUE_COUNT 6
#define DEFAULT_BASS_FILTER_INDEX 3

static const float soft_limiter_thresholds[] = {0.0f, 0.7f, 0.6f, 0.5f};
#define SOFT_LIMITER_VALUE_COUNT 4
#define DEFAULT_SOFT_LIMITER_INDEX 2

static struct {
    int screen_off_timeout;
    bool lyrics_enabled;
    int bass_filter_hz;
    int soft_limiter_index;
    int max_playlists;
    int playlist_scan_depth;
} current_settings;

static int clamp_max_playlists(int value) {
    if (value < 1) return 1;
    if (value > MAX_PLAYLISTS_CAP) return MAX_PLAYLISTS_CAP;
    return value;
}

static int clamp_playlist_scan_depth(int value) {
    if (value < 1) return 1;
    if (value > MAX_PLAYLIST_SCAN_DEPTH_CAP) return MAX_PLAYLIST_SCAN_DEPTH_CAP;
    return value;
}

static void apply_config_line(const char* line) {
    int value;
    if (sscanf(line, "screen_off_timeout=%d", &value) == 1) {
        for (int i = 0; i < SCREEN_OFF_VALUE_COUNT; i++) {
            if (screen_off_values[i] == value) {
                current_settings.screen_off_timeout = value;
                break;
            }
        }
    }
    if (sscanf(line, "lyrics_enabled=%d", &value) == 1) {
        current_settings.lyrics_enabled = (value != 0);
    }
    if (sscanf(line, "bass_filter_hz=%d", &value) == 1) {
        for (int i = 0; i < BASS_FILTER_VALUE_COUNT; i++) {
            if (bass_filter_values[i] == value) {
                current_settings.bass_filter_hz = value;
                break;
            }
        }
    }
    if (sscanf(line, "soft_limiter=%d", &value) == 1) {
        if (value >= 0 && value < SOFT_LIMITER_VALUE_COUNT) {
            current_settings.soft_limiter_index = value;
        }
    }
    if (sscanf(line, "max_playlists=%d", &value) == 1) {
        current_settings.max_playlists = clamp_max_playlists(value);
    }
    if (sscanf(line, "playlist_scan_depth=%d", &value) == 1) {
        current_settings.playlist_scan_depth = clamp_playlist_scan_depth(value);
    }
}

static void load_config_file(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        apply_config_line(line);
    }
    fclose(f);
}

static int get_screen_off_index(void) {
    for (int i = 0; i < SCREEN_OFF_VALUE_COUNT; i++) {
        if (screen_off_values[i] == current_settings.screen_off_timeout) {
            return i;
        }
    }
    return DEFAULT_SCREEN_OFF_INDEX;
}

static int get_bass_filter_index(void) {
    for (int i = 0; i < BASS_FILTER_VALUE_COUNT; i++) {
        if (bass_filter_values[i] == current_settings.bass_filter_hz) {
            return i;
        }
    }
    return DEFAULT_BASS_FILTER_INDEX;
}

void Settings_init(void) {
    current_settings.screen_off_timeout = screen_off_values[DEFAULT_SCREEN_OFF_INDEX];
    current_settings.lyrics_enabled = true;
    current_settings.bass_filter_hz = bass_filter_values[DEFAULT_BASS_FILTER_INDEX];
    current_settings.soft_limiter_index = DEFAULT_SOFT_LIMITER_INDEX;
    current_settings.max_playlists = DEFAULT_MAX_PLAYLISTS;
    current_settings.playlist_scan_depth = DEFAULT_PLAYLIST_SCAN_DEPTH;

    load_config_file(SETTINGS_FILE);
    load_config_file(OVERRIDES_FILE);
}

void Settings_quit(void) {
    Settings_save();
}

int Settings_getScreenOffTimeout(void) {
    return current_settings.screen_off_timeout;
}

void Settings_setScreenOffTimeout(int seconds) {
    for (int i = 0; i < SCREEN_OFF_VALUE_COUNT; i++) {
        if (screen_off_values[i] == seconds) {
            current_settings.screen_off_timeout = seconds;
            Settings_save();
            return;
        }
    }
}

void Settings_cycleScreenOffNext(void) {
    int index = get_screen_off_index();
    index = (index + 1) % SCREEN_OFF_VALUE_COUNT;
    current_settings.screen_off_timeout = screen_off_values[index];
    Settings_save();
}

void Settings_cycleScreenOffPrev(void) {
    int index = get_screen_off_index();
    index = (index - 1 + SCREEN_OFF_VALUE_COUNT) % SCREEN_OFF_VALUE_COUNT;
    current_settings.screen_off_timeout = screen_off_values[index];
    Settings_save();
}

const char* Settings_getScreenOffDisplayStr(void) {
    switch (current_settings.screen_off_timeout) {
        case 60:  return "60s";
        case 90:  return "90s";
        case 120: return "120s";
        case 0:   return "Off";
        default:  return "60s";
    }
}

void Settings_save(void) {
    char mkdir_cmd[512];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p %s", SETTINGS_DIR);
    system(mkdir_cmd);

    FILE* f = fopen(SETTINGS_FILE, "w");
    if (!f) return;

    fprintf(f, "screen_off_timeout=%d\n", current_settings.screen_off_timeout);
    fprintf(f, "lyrics_enabled=%d\n", current_settings.lyrics_enabled ? 1 : 0);
    fprintf(f, "bass_filter_hz=%d\n", current_settings.bass_filter_hz);
    fprintf(f, "soft_limiter=%d\n", current_settings.soft_limiter_index);
    fclose(f);
}

int Settings_getMaxPlaylists(void) {
    return current_settings.max_playlists;
}

int Settings_getPlaylistScanDepth(void) {
    return current_settings.playlist_scan_depth;
}

bool Settings_getLyricsEnabled(void) {
    return current_settings.lyrics_enabled;
}

void Settings_setLyricsEnabled(bool enabled) {
    current_settings.lyrics_enabled = enabled;
    Settings_save();
}

void Settings_toggleLyrics(void) {
    current_settings.lyrics_enabled = !current_settings.lyrics_enabled;
    Settings_save();
}

int Settings_getBassFilterHz(void) {
    return current_settings.bass_filter_hz;
}

void Settings_cycleBassFilterNext(void) {
    int index = get_bass_filter_index();
    index = (index + 1) % BASS_FILTER_VALUE_COUNT;
    current_settings.bass_filter_hz = bass_filter_values[index];
    Settings_save();
}

void Settings_cycleBassFilterPrev(void) {
    int index = get_bass_filter_index();
    index = (index - 1 + BASS_FILTER_VALUE_COUNT) % BASS_FILTER_VALUE_COUNT;
    current_settings.bass_filter_hz = bass_filter_values[index];
    Settings_save();
}

const char* Settings_getBassFilterDisplayStr(void) {
    static char buf[16];
    if (current_settings.bass_filter_hz == 0) return "Off";
    snprintf(buf, sizeof(buf), "%d Hz", current_settings.bass_filter_hz);
    return buf;
}

int Settings_getSoftLimiter(void) {
    return current_settings.soft_limiter_index;
}

float Settings_getSoftLimiterThreshold(void) {
    if (current_settings.soft_limiter_index >= 0 && current_settings.soft_limiter_index < SOFT_LIMITER_VALUE_COUNT) {
        return soft_limiter_thresholds[current_settings.soft_limiter_index];
    }
    return soft_limiter_thresholds[DEFAULT_SOFT_LIMITER_INDEX];
}

void Settings_cycleSoftLimiterNext(void) {
    current_settings.soft_limiter_index = (current_settings.soft_limiter_index + 1) % SOFT_LIMITER_VALUE_COUNT;
    Settings_save();
}

void Settings_cycleSoftLimiterPrev(void) {
    current_settings.soft_limiter_index = (current_settings.soft_limiter_index - 1 + SOFT_LIMITER_VALUE_COUNT) % SOFT_LIMITER_VALUE_COUNT;
    Settings_save();
}

const char* Settings_getSoftLimiterDisplayStr(void) {
    switch (current_settings.soft_limiter_index) {
        case 0:  return "Off";
        case 1:  return "Mild";
        case 2:  return "Medium";
        case 3:  return "Strong";
        default: return "Medium";
    }
}
