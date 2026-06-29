#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include "defines.h"
#include "api.h"
#include "playlist_m3u.h"
#include "player.h"

void M3U_init(void) {
    mkdir(SHARED_USERDATA_PATH "/music-player", 0755);
    mkdir(PLAYLISTS_DIR, 0755);
}

static int count_tracks_in_file(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return 0;

    int count = 0;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        int len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len == 0 || line[0] == '#') continue;
        count++;
    }
    fclose(f);
    return count;
}

static void sort_playlists_by_name(PlaylistInfo* items, int count) {
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (strcasecmp(items[i].name, items[j].name) > 0) {
                PlaylistInfo tmp = items[i];
                items[i] = items[j];
                items[j] = tmp;
            }
        }
    }
}

int M3U_dirDepth(const char* dir) {
    if (!dir) return 0;

    size_t base_len = strlen(PLAYLISTS_DIR);
    if (strncmp(dir, PLAYLISTS_DIR, base_len) != 0) return 0;

    const char* rel = dir + base_len;
    if (*rel == '/') rel++;
    if (*rel == '\0') return 0;

    int depth = 0;
    for (const char* p = rel; *p; p++) {
        if (*p == '/') depth++;
    }
    return depth + 1;
}

bool M3U_isPlaylistsRoot(const char* dir) {
    return dir && strcmp(dir, PLAYLISTS_DIR) == 0;
}

int M3U_parentDirectory(const char* dir, char* out, int out_size) {
    if (!dir || !out || out_size <= 0) return -1;

    if (M3U_isPlaylistsRoot(dir)) {
        snprintf(out, out_size, "%s", PLAYLISTS_DIR);
        return 0;
    }

    const char* slash = strrchr(dir, '/');
    if (!slash) return -1;

    size_t len = (size_t)(slash - dir);
    if (len >= (size_t)out_size) return -1;

    memcpy(out, dir, len);
    out[len] = '\0';

    if (strncmp(out, PLAYLISTS_DIR, strlen(PLAYLISTS_DIR)) != 0) {
        snprintf(out, out_size, "%s", PLAYLISTS_DIR);
    }
    return 0;
}

int M3U_listDirectory(const char* dir, PlaylistInfo* out, int max, int max_scan_depth) {
    if (!dir || !out || max <= 0) return 0;

    DIR* d = opendir(dir);
    if (!d) return 0;

    bool allow_folders = (M3U_dirDepth(dir) < max_scan_depth);

    PlaylistInfo* folders = calloc((size_t)max, sizeof(PlaylistInfo));
    PlaylistInfo* playlists = calloc((size_t)max, sizeof(PlaylistInfo));
    if (!folders || !playlists) {
        free(folders);
        free(playlists);
        closedir(d);
        return 0;
    }

    int folder_count = 0;
    int playlist_count = 0;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir, ent->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (!allow_folders || folder_count >= max) continue;
            PlaylistInfo* info = &folders[folder_count++];
            snprintf(info->path, sizeof(info->path), "%s", full_path);
            snprintf(info->name, sizeof(info->name), "%s", ent->d_name);
            info->is_folder = true;
            info->track_count = 0;
        } else {
            int len = (int)strlen(ent->d_name);
            if (len < 5 || strcasecmp(ent->d_name + len - 4, ".m3u") != 0) continue;
            if (playlist_count >= max) continue;

            PlaylistInfo* info = &playlists[playlist_count++];
            snprintf(info->path, sizeof(info->path), "%s", full_path);
            snprintf(info->name, sizeof(info->name), "%.*s", len - 4, ent->d_name);
            info->is_folder = false;
            info->track_count = count_tracks_in_file(full_path);
        }
    }
    closedir(d);

    sort_playlists_by_name(folders, folder_count);
    sort_playlists_by_name(playlists, playlist_count);

    int count = 0;
    for (int i = 0; i < folder_count && count < max; i++) {
        out[count++] = folders[i];
    }
    for (int i = 0; i < playlist_count && count < max; i++) {
        out[count++] = playlists[i];
    }

    free(folders);
    free(playlists);
    return count;
}

static void append_m3u_entry(PlaylistInfo* out, int max, int* count,
                             const char* full_path, const char* fname) {
    if (*count >= max) return;

    int len = (int)strlen(fname);
    if (len < 5 || strcasecmp(fname + len - 4, ".m3u") != 0) return;

    PlaylistInfo* info = &out[*count];
    snprintf(info->path, sizeof(info->path), "%s", full_path);
    snprintf(info->name, sizeof(info->name), "%.*s", len - 4, fname);
    info->is_folder = false;
    info->track_count = count_tracks_in_file(full_path);
    (*count)++;
}

static void list_all_recursive(const char* dir, int max_scan_depth,
                               PlaylistInfo* out, int max, int* count) {
    if (!dir || !out || !count || *count >= max) return;

    DIR* d = opendir(dir);
    if (!d) return;

    int depth = M3U_dirDepth(dir);
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL && *count < max) {
        if (ent->d_name[0] == '.') continue;

        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir, ent->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (depth < max_scan_depth) {
                list_all_recursive(full_path, max_scan_depth, out, max, count);
            }
            continue;
        }

        int len = (int)strlen(ent->d_name);
        if (len < 5 || strcasecmp(ent->d_name + len - 4, ".m3u") != 0) continue;

        append_m3u_entry(out, max, count, full_path, ent->d_name);
    }
    closedir(d);
}

static int compare_by_name(const void* a, const void* b) {
    const PlaylistInfo* pa = (const PlaylistInfo*)a;
    const PlaylistInfo* pb = (const PlaylistInfo*)b;
    return strcasecmp(pa->name, pb->name);
}

static int compare_by_path(const void* a, const void* b) {
    const PlaylistInfo* pa = (const PlaylistInfo*)a;
    const PlaylistInfo* pb = (const PlaylistInfo*)b;
    return strcasecmp(pa->path, pb->path);
}

static void collect_root_playlists(PlaylistInfo* out, int max, int* count) {
    DIR* d = opendir(PLAYLISTS_DIR);
    if (!d) return;

    struct dirent* ent;
    while ((ent = readdir(d)) != NULL && *count < max) {
        if (ent->d_name[0] == '.') continue;

        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", PLAYLISTS_DIR, ent->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        append_m3u_entry(out, max, count, full_path, ent->d_name);
    }
    closedir(d);
}

static bool is_root_playlist_path(const char* m3u_path) {
    if (!m3u_path) return false;

    const char* slash = strrchr(m3u_path, '/');
    if (!slash) return false;

    char dir[512];
    size_t dir_len = (size_t)(slash - m3u_path);
    if (dir_len >= sizeof(dir)) return false;

    memcpy(dir, m3u_path, dir_len);
    dir[dir_len] = '\0';
    return M3U_isPlaylistsRoot(dir);
}

static int compare_picker_entries(const void* a, const void* b) {
    return compare_by_path(a, b);
}

int M3U_listAllPlaylists(PlaylistInfo* out, int max, int max_scan_depth) {
    if (!out || max <= 0) return 0;
    int count = 0;
    list_all_recursive(PLAYLISTS_DIR, max_scan_depth, out, max, &count);
    sort_playlists_by_name(out, count);
    return count;
}

int M3U_listPlaylistsForPicker(PlaylistInfo* out, int max, int max_scan_depth) {
    if (!out || max <= 0) return 0;

    int count = 0;
    collect_root_playlists(out, max, &count);
    int root_count = count;
    if (root_count > 1) {
        qsort(out, (size_t)root_count, sizeof(PlaylistInfo), compare_by_name);
    }

    if (max_scan_depth > 0 && count < max) {
        DIR* d = opendir(PLAYLISTS_DIR);
        if (d) {
            struct dirent* ent;
            while ((ent = readdir(d)) != NULL && count < max) {
                if (ent->d_name[0] == '.') continue;

                char full_path[512];
                snprintf(full_path, sizeof(full_path), "%s/%s", PLAYLISTS_DIR, ent->d_name);

                struct stat st;
                if (stat(full_path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

                list_all_recursive(full_path, max_scan_depth, out, max, &count);
            }
            closedir(d);
        }
    }

    if (count > root_count) {
        qsort(out + root_count, (size_t)(count - root_count), sizeof(PlaylistInfo), compare_picker_entries);
    }
    return count;
}

void M3U_formatPickerLabel(const PlaylistInfo* info, char* out, int out_size) {
    if (!info || !out || out_size <= 0) return;

    if (is_root_playlist_path(info->path)) {
        snprintf(out, out_size, "%s (%d)", info->name, info->track_count);
        return;
    }

    const char* rel = info->path + strlen(PLAYLISTS_DIR);
    if (*rel == '/') rel++;

    char path_part[256];
    snprintf(path_part, sizeof(path_part), "%s", rel);
    char* dot = strrchr(path_part, '.');
    if (dot && dot != path_part) *dot = '\0';

    char pretty[256];
    int pi = 0;
    for (int i = 0; path_part[i] && pi < (int)sizeof(pretty) - 4; i++) {
        if (path_part[i] == '/') {
            if (pi > 0) {
                pretty[pi++] = ' ';
                pretty[pi++] = '/';
                pretty[pi++] = ' ';
            }
        } else {
            pretty[pi++] = path_part[i];
        }
    }
    pretty[pi] = '\0';

    snprintf(out, out_size, "%s (%d)", pretty, info->track_count);
}

int M3U_createAt(const char* dir, const char* name) {
    if (!dir || !name || !name[0]) return -1;

    M3U_init();

    char path[512];
    snprintf(path, sizeof(path), "%s/%s.m3u", dir, name);

    if (access(path, F_OK) == 0) return -1;

    FILE* f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f, "#EXTM3U\n");
    fclose(f);
    return 0;
}

int M3U_create(const char* name) {
    return M3U_createAt(PLAYLISTS_DIR, name);
}

int M3U_delete(const char* m3u_path) {
    if (!m3u_path) return -1;
    return unlink(m3u_path);
}

int M3U_addTrack(const char* m3u_path, const char* track_path, const char* display_name) {
    if (!m3u_path || !track_path) return -1;

    if (M3U_containsTrack(m3u_path, track_path)) return -1;

    FILE* f = fopen(m3u_path, "a");
    if (!f) return -1;

    const char* name = display_name ? display_name : track_path;
    fprintf(f, "#EXTINF:0,%s\n%s\n", name, track_path);
    fclose(f);
    return 0;
}

int M3U_removeTrack(const char* m3u_path, int index) {
    if (!m3u_path || index < 0) return -1;

    FILE* f = fopen(m3u_path, "r");
    if (!f) return -1;

    char** lines = NULL;
    int line_count = 0;
    int capacity = 64;
    lines = malloc(sizeof(char*) * capacity);
    if (!lines) { fclose(f); return -1; }

    char buf[1024];
    while (fgets(buf, sizeof(buf), f)) {
        if (line_count >= capacity) {
            capacity *= 2;
            char** new_lines = realloc(lines, sizeof(char*) * capacity);
            if (!new_lines) {
                for (int i = 0; i < line_count; i++) free(lines[i]);
                free(lines);
                fclose(f);
                return -1;
            }
            lines = new_lines;
        }
        lines[line_count] = strdup(buf);
        if (lines[line_count]) line_count++;
    }
    fclose(f);

    int track_idx = 0;
    int remove_line = -1;
    for (int i = 0; i < line_count; i++) {
        char* line = lines[i];
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            len--;
        if (len == 0 || line[0] == '#') continue;

        if (track_idx == index) {
            remove_line = i;
            break;
        }
        track_idx++;
    }

    if (remove_line < 0) {
        for (int i = 0; i < line_count; i++) free(lines[i]);
        free(lines);
        return -1;
    }

    f = fopen(m3u_path, "w");
    if (!f) {
        for (int i = 0; i < line_count; i++) free(lines[i]);
        free(lines);
        return -1;
    }

    for (int i = 0; i < line_count; i++) {
        if (i == remove_line) continue;
        if (i == remove_line - 1 && lines[i][0] == '#' &&
            strncmp(lines[i], "#EXTINF", 7) == 0) continue;
        fputs(lines[i], f);
    }
    fclose(f);

    for (int i = 0; i < line_count; i++) free(lines[i]);
    free(lines);
    return 0;
}

int M3U_loadTracks(const char* m3u_path, PlaylistTrack* tracks, int max, int* count) {
    if (!m3u_path || !tracks || !count) return -1;
    *count = 0;

    FILE* f = fopen(m3u_path, "r");
    if (!f) return -1;

    char line[1024];
    char last_extinf_name[256] = "";

    while (fgets(line, sizeof(line), f) && *count < max) {
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        if (len == 0) continue;

        if (strncmp(line, "#EXTINF:", 8) == 0) {
            char* comma = strchr(line + 8, ',');
            if (comma) {
                snprintf(last_extinf_name, sizeof(last_extinf_name), "%s", comma + 1);
            }
            continue;
        }

        if (line[0] == '#') continue;

        if (access(line, F_OK) != 0) {
            last_extinf_name[0] = '\0';
            continue;
        }

        PlaylistTrack* track = &tracks[*count];
        snprintf(track->path, sizeof(track->path), "%s", line);

        if (last_extinf_name[0]) {
            snprintf(track->name, sizeof(track->name), "%s", last_extinf_name);
        } else {
            const char* slash = strrchr(line, '/');
            const char* fname = slash ? slash + 1 : line;
            snprintf(track->name, sizeof(track->name), "%s", fname);
        }

        track->format = Player_detectFormat(line);
        last_extinf_name[0] = '\0';
        (*count)++;
    }

    fclose(f);
    return 0;
}

bool M3U_containsTrack(const char* m3u_path, const char* track_path) {
    if (!m3u_path || !track_path) return false;

    FILE* f = fopen(m3u_path, "r");
    if (!f) return false;

    char line[1024];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len == 0 || line[0] == '#') continue;

        if (strcmp(line, track_path) == 0) {
            found = true;
            break;
        }
    }

    fclose(f);
    return found;
}
