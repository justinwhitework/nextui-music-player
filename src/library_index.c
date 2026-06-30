#define _GNU_SOURCE
#include "library_index.h"
#include "metadata_reader.h"
#include "playlist_m3u.h"
#include "playlist.h"
#include "player.h"
#include "settings.h"
#include "browser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdint.h>

#include "defines.h"
#include "api.h"
#include "include/parson/parson.h"

#define MUSIC_PATH SDCARD_PATH "/Music"
#define INDEX_PATH SHARED_USERDATA_PATH "/music-player/library_index.json"
#define LIBRARY_MAX_TRACKS 32768
#define LIBRARY_MAX_TOKENS 65536
#define TOKEN_GROW 16

typedef struct {
    char path[512];
    char title[256];
    char artist[256];
    char album[256];
    char genre[256];
    char filename_norm[256];
    AudioFormat format;
} IndexedTrack;

typedef struct {
    char path[512];
    char name[128];
    char name_norm[128];
    char parent_folder[64];
    char parent_norm[64];
    int track_count;
    bool is_root;
} IndexedPlaylist;

typedef struct {
    char token[48];
    int* track_ids;
    int track_count;
    int track_cap;
    int* playlist_ids;
    int playlist_count;
    int playlist_cap;
} IndexToken;

typedef struct {
    uint64_t file_count;
    uint64_t latest_mtime;
} LibraryFingerprint;

static pthread_mutex_t index_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool build_started = false;
static bool build_running = false;
static bool index_ready = false;
static char build_status[128] = "Starting...";

static IndexedTrack* tracks = NULL;
static int track_count = 0;
static IndexedPlaylist* playlists = NULL;
static int playlist_count = 0;
static IndexToken* tokens = NULL;
static int token_count = 0;
static int token_cap = 0;

static char saved_fingerprint[64] = "";

static void set_status(const char* msg) {
    pthread_mutex_lock(&index_mutex);
    snprintf(build_status, sizeof(build_status), "%s", msg ? msg : "");
    pthread_mutex_unlock(&index_mutex);
}

static void fingerprint_to_string(const LibraryFingerprint* fp, char* out, int out_size) {
    snprintf(out, out_size, "%llu_%llu",
             (unsigned long long)fp->file_count,
             (unsigned long long)fp->latest_mtime);
}

static void update_fp(LibraryFingerprint* fp, const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return;
    fp->file_count++;
    if ((uint64_t)st.st_mtime > fp->latest_mtime) {
        fp->latest_mtime = (uint64_t)st.st_mtime;
    }
}

static void scan_fp_dir(const char* dir, LibraryFingerprint* fp, bool playlists_only) {
    DIR* d = opendir(dir);
    if (!d) return;

    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char full[768];
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);

        struct stat st;
        if (lstat(full, &st) != 0) continue;
        if (S_ISLNK(st.st_mode)) continue;

        if (S_ISDIR(st.st_mode)) {
            scan_fp_dir(full, fp, playlists_only);
            continue;
        }

        if (playlists_only) {
            size_t len = strlen(ent->d_name);
            if (len > 4 && strcasecmp(ent->d_name + len - 4, ".m3u") == 0) {
                update_fp(fp, full);
            }
        } else if (Browser_isAudioFile(ent->d_name)) {
            update_fp(fp, full);
        }
    }
    closedir(d);
}

static void compute_fingerprint(LibraryFingerprint* fp) {
    memset(fp, 0, sizeof(*fp));
    scan_fp_dir(MUSIC_PATH, fp, false);
    scan_fp_dir(PLAYLISTS_DIR, fp, true);
}

static void free_index_data(void) {
    for (int i = 0; i < token_count; i++) {
        free(tokens[i].track_ids);
        free(tokens[i].playlist_ids);
    }
    free(tokens);
    tokens = NULL;
    token_count = 0;
    token_cap = 0;

    free(tracks);
    tracks = NULL;
    track_count = 0;

    free(playlists);
    playlists = NULL;
    playlist_count = 0;
}

static int find_token(const char* token) {
    for (int i = 0; i < token_count; i++) {
        if (strcmp(tokens[i].token, token) == 0) return i;
    }
    return -1;
}

static int ensure_token(const char* token) {
    if (!token || !token[0]) return -1;

    int idx = find_token(token);
    if (idx >= 0) return idx;

    if (token_count >= LIBRARY_MAX_TOKENS) return -1;

    if (token_count >= token_cap) {
        int nc = token_cap ? token_cap * 2 : 4096;
        IndexToken* nt = realloc(tokens, sizeof(IndexToken) * nc);
        if (!nt) return -1;
        memset(nt + token_cap, 0, sizeof(IndexToken) * (nc - token_cap));
        tokens = nt;
        token_cap = nc;
    }

    idx = token_count++;
    memset(&tokens[idx], 0, sizeof(tokens[idx]));
    strncpy(tokens[idx].token, token, sizeof(tokens[idx].token) - 1);
    return idx;
}

static void token_add_track(int token_idx, int track_id) {
    if (token_idx < 0 || track_id < 0) return;
    IndexToken* t = &tokens[token_idx];

    for (int i = 0; i < t->track_count; i++) {
        if (t->track_ids[i] == track_id) return;
    }

    if (t->track_count >= t->track_cap) {
        int nc = t->track_cap ? t->track_cap * 2 : TOKEN_GROW;
        int* nd = realloc(t->track_ids, sizeof(int) * nc);
        if (!nd) return;
        t->track_ids = nd;
        t->track_cap = nc;
    }
    t->track_ids[t->track_count++] = track_id;
}

static void token_add_playlist(int token_idx, int playlist_id) {
    if (token_idx < 0 || playlist_id < 0) return;
    IndexToken* t = &tokens[token_idx];

    for (int i = 0; i < t->playlist_count; i++) {
        if (t->playlist_ids[i] == playlist_id) return;
    }

    if (t->playlist_count >= t->playlist_cap) {
        int nc = t->playlist_cap ? t->playlist_cap * 2 : TOKEN_GROW;
        int* nd = realloc(t->playlist_ids, sizeof(int) * nc);
        if (!nd) return;
        t->playlist_ids = nd;
        t->playlist_cap = nc;
    }
    t->playlist_ids[t->playlist_count++] = playlist_id;
}

typedef struct {
    int id;
    bool is_track;
} TokenAddCtx;

static void add_token_cb(const char* token, void* userdata) {
    TokenAddCtx* ctx = (TokenAddCtx*)userdata;
    int ti = ensure_token(token);
    if (ti < 0) return;
    if (ctx->is_track) token_add_track(ti, ctx->id);
    else token_add_playlist(ti, ctx->id);
}

static void index_track_tokens(int id) {
    if (id < 0 || id >= track_count) return;
    IndexedTrack* tr = &tracks[id];
    TokenAddCtx ctx = {.id = id, .is_track = true};

    Metadata_foreachToken(tr->title, add_token_cb, &ctx);
    Metadata_foreachToken(tr->artist, add_token_cb, &ctx);
    Metadata_foreachToken(tr->album, add_token_cb, &ctx);
    Metadata_foreachToken(tr->genre, add_token_cb, &ctx);
    Metadata_foreachToken(tr->filename_norm, add_token_cb, &ctx);
}

static void index_playlist_tokens(int id) {
    if (id < 0 || id >= playlist_count) return;
    IndexedPlaylist* pl = &playlists[id];
    TokenAddCtx ctx = {.id = id, .is_track = false};

    Metadata_foreachToken(pl->name_norm, add_token_cb, &ctx);
    if (!pl->is_root) {
        Metadata_foreachToken(pl->parent_norm, add_token_cb, &ctx);
    }
}

static bool load_json_index(const char* expected_fp) {
    JSON_Value* root = json_parse_file(INDEX_PATH);
    if (!root) return false;

    JSON_Object* obj = json_value_get_object(root);
    const char* fp = json_object_get_string(obj, "fingerprint");
    if (!fp || strcmp(fp, expected_fp) != 0) {
        json_value_free(root);
        return false;
    }

    free_index_data();

    JSON_Array* track_arr = json_object_get_array(obj, "tracks");
    if (track_arr) {
        size_t n = json_array_get_count(track_arr);
        if (n > (size_t)LIBRARY_MAX_TRACKS) n = LIBRARY_MAX_TRACKS;
        tracks = calloc(n, sizeof(IndexedTrack));
        if (!tracks) {
            json_value_free(root);
            return false;
        }
        for (size_t i = 0; i < n; i++) {
            JSON_Object* to = json_array_get_object(track_arr, i);
            if (!to) continue;
            const char* path = json_object_get_string(to, "path");
            if (path) strncpy(tracks[track_count].path, path, sizeof(tracks[track_count].path) - 1);
            const char* title = json_object_get_string(to, "title");
            if (title) strncpy(tracks[track_count].title, title, sizeof(tracks[track_count].title) - 1);
            const char* artist = json_object_get_string(to, "artist");
            if (artist) strncpy(tracks[track_count].artist, artist, sizeof(tracks[track_count].artist) - 1);
            const char* album = json_object_get_string(to, "album");
            if (album) strncpy(tracks[track_count].album, album, sizeof(tracks[track_count].album) - 1);
            const char* genre = json_object_get_string(to, "genre");
            if (genre) strncpy(tracks[track_count].genre, genre, sizeof(tracks[track_count].genre) - 1);
            const char* fn = json_object_get_string(to, "filename_norm");
            if (fn) strncpy(tracks[track_count].filename_norm, fn, sizeof(tracks[track_count].filename_norm) - 1);
            tracks[track_count].format = (AudioFormat)json_object_get_number(to, "format");
            track_count++;
        }
    }

    JSON_Array* pl_arr = json_object_get_array(obj, "playlists");
    if (pl_arr) {
        size_t n = json_array_get_count(pl_arr);
        playlists = calloc(n, sizeof(IndexedPlaylist));
        if (!playlists) {
            free_index_data();
            json_value_free(root);
            return false;
        }
        for (size_t i = 0; i < n; i++) {
            JSON_Object* po = json_array_get_object(pl_arr, i);
            if (!po) continue;
            const char* path = json_object_get_string(po, "path");
            if (path) strncpy(playlists[playlist_count].path, path, sizeof(playlists[playlist_count].path) - 1);
            const char* name = json_object_get_string(po, "name");
            if (name) strncpy(playlists[playlist_count].name, name, sizeof(playlists[playlist_count].name) - 1);
            const char* nn = json_object_get_string(po, "name_norm");
            if (nn) strncpy(playlists[playlist_count].name_norm, nn, sizeof(playlists[playlist_count].name_norm) - 1);
            const char* pf = json_object_get_string(po, "parent_folder");
            if (pf) strncpy(playlists[playlist_count].parent_folder, pf, sizeof(playlists[playlist_count].parent_folder) - 1);
            const char* pn = json_object_get_string(po, "parent_norm");
            if (pn) strncpy(playlists[playlist_count].parent_norm, pn, sizeof(playlists[playlist_count].parent_norm) - 1);
            playlists[playlist_count].track_count = (int)json_object_get_number(po, "track_count");
            playlists[playlist_count].is_root = json_object_get_boolean(po, "is_root");
            playlist_count++;
        }
    }

    JSON_Object* tok_obj = json_object_get_object(obj, "tokens");
    if (tok_obj) {
        size_t key_count = json_object_get_count(tok_obj);
        tokens = calloc(key_count ? key_count : 1, sizeof(IndexToken));
        if (!tokens) {
            free_index_data();
            json_value_free(root);
            return false;
        }
        for (size_t i = 0; i < key_count; i++) {
            const char* key = json_object_get_name(tok_obj, i);
            JSON_Object* to = json_object_get_object(tok_obj, key);
            if (!key || !to) continue;

            strncpy(tokens[token_count].token, key, sizeof(tokens[token_count].token) - 1);

            JSON_Array* ta = json_object_get_array(to, "tracks");
            if (ta) {
                size_t tn = json_array_get_count(ta);
                tokens[token_count].track_ids = calloc(tn ? tn : 1, sizeof(int));
                if (tokens[token_count].track_ids) {
                    tokens[token_count].track_cap = (int)tn;
                    for (size_t j = 0; j < tn; j++) {
                        tokens[token_count].track_ids[tokens[token_count].track_count++] =
                            (int)json_array_get_number(ta, j);
                    }
                }
            }

            JSON_Array* pa = json_object_get_array(to, "playlists");
            if (pa) {
                size_t pn = json_array_get_count(pa);
                tokens[token_count].playlist_ids = calloc(pn ? pn : 1, sizeof(int));
                if (tokens[token_count].playlist_ids) {
                    tokens[token_count].playlist_cap = (int)pn;
                    for (size_t j = 0; j < pn; j++) {
                        tokens[token_count].playlist_ids[tokens[token_count].playlist_count++] =
                            (int)json_array_get_number(pa, j);
                    }
                }
            }
            token_count++;
        }
    }

    strncpy(saved_fingerprint, expected_fp, sizeof(saved_fingerprint) - 1);
    json_value_free(root);
    return true;
}

static int save_json_index(const char* fp_str) {
    mkdir(SHARED_USERDATA_PATH "/music-player", 0755);

    JSON_Value* root = json_value_init_object();
    if (!root) return -1;
    JSON_Object* obj = json_value_get_object(root);

    json_object_set_string(obj, "fingerprint", fp_str);

    JSON_Value* track_arr_val = json_value_init_array();
    JSON_Array* track_arr = json_value_get_array(track_arr_val);
    for (int i = 0; i < track_count; i++) {
        JSON_Value* tv = json_value_init_object();
        JSON_Object* to = json_value_get_object(tv);
        json_object_set_string(to, "path", tracks[i].path);
        json_object_set_string(to, "title", tracks[i].title);
        json_object_set_string(to, "artist", tracks[i].artist);
        json_object_set_string(to, "album", tracks[i].album);
        json_object_set_string(to, "genre", tracks[i].genre);
        json_object_set_string(to, "filename_norm", tracks[i].filename_norm);
        json_object_set_number(to, "format", tracks[i].format);
        json_array_append_value(track_arr, tv);
    }
    json_object_set_value(obj, "tracks", track_arr_val);

    JSON_Value* pl_arr_val = json_value_init_array();
    JSON_Array* pl_arr = json_value_get_array(pl_arr_val);
    for (int i = 0; i < playlist_count; i++) {
        JSON_Value* pv = json_value_init_object();
        JSON_Object* po = json_value_get_object(pv);
        json_object_set_string(po, "path", playlists[i].path);
        json_object_set_string(po, "name", playlists[i].name);
        json_object_set_string(po, "name_norm", playlists[i].name_norm);
        json_object_set_string(po, "parent_folder", playlists[i].parent_folder);
        json_object_set_string(po, "parent_norm", playlists[i].parent_norm);
        json_object_set_number(po, "track_count", playlists[i].track_count);
        json_object_set_boolean(po, "is_root", playlists[i].is_root);
        json_array_append_value(pl_arr, pv);
    }
    json_object_set_value(obj, "playlists", pl_arr_val);

    JSON_Value* tok_val = json_value_init_object();
    JSON_Object* tok_obj = json_value_get_object(tok_val);
    for (int i = 0; i < token_count; i++) {
        JSON_Value* entry_val = json_value_init_object();
        JSON_Object* entry = json_value_get_object(entry_val);

        JSON_Value* ta_val = json_value_init_array();
        JSON_Array* ta = json_value_get_array(ta_val);
        for (int j = 0; j < tokens[i].track_count; j++) {
            json_array_append_number(ta, tokens[i].track_ids[j]);
        }
        json_object_set_value(entry, "tracks", ta_val);

        JSON_Value* pa_val = json_value_init_array();
        JSON_Array* pa = json_value_get_array(pa_val);
        for (int j = 0; j < tokens[i].playlist_count; j++) {
            json_array_append_number(pa, tokens[i].playlist_ids[j]);
        }
        json_object_set_value(entry, "playlists", pa_val);

        json_object_set_value(tok_obj, tokens[i].token, entry_val);
    }
    json_object_set_value(obj, "tokens", tok_val);

    int rc = json_serialize_to_file_pretty(root, INDEX_PATH);
    json_value_free(root);
    return rc == JSONSuccess ? 0 : -1;
}

static void rebuild_index(const char* fp_str) {
    free_index_data();

    set_status("Scanning music...");
  {
        char** paths = NULL;
        int count = Playlist_collectPaths(MUSIC_PATH, &paths, LIBRARY_MAX_TRACKS);
        if (count > 0 && paths) {
            tracks = calloc(count, sizeof(IndexedTrack));
            if (tracks) {
                for (int i = 0; i < count; i++) {
                    set_status("Reading tags...");
                    IndexedTrack* tr = &tracks[track_count];
                    strncpy(tr->path, paths[i], sizeof(tr->path) - 1);

                    TrackMetadata meta;
                    Metadata_readFromFile(paths[i], &meta);
                    strncpy(tr->title, meta.title, sizeof(tr->title) - 1);
                    strncpy(tr->artist, meta.artist, sizeof(tr->artist) - 1);
                    strncpy(tr->album, meta.album, sizeof(tr->album) - 1);
                    strncpy(tr->genre, meta.genre, sizeof(tr->genre) - 1);
                    tr->format = Player_detectFormat(paths[i]);

                    const char* base = strrchr(paths[i], '/');
                    base = base ? base + 1 : paths[i];
                    Metadata_copyNormalized(tr->filename_norm, sizeof(tr->filename_norm), base);

                    track_count++;
                }
            }
            Playlist_freePaths(paths, count);
        }
    }

    set_status("Scanning playlists...");
    {
        int limit = Settings_getMaxPlaylists();
        PlaylistInfo* infos = calloc(limit, sizeof(PlaylistInfo));
        if (infos) {
            int n = M3U_listAllPlaylists(infos, limit, Settings_getPlaylistScanDepth());
            if (n > 0) {
                playlists = calloc(n, sizeof(IndexedPlaylist));
                if (playlists) {
                    for (int i = 0; i < n; i++) {
                        if (infos[i].is_folder) continue;

                        IndexedPlaylist* pl = &playlists[playlist_count];
                        strncpy(pl->path, infos[i].path, sizeof(pl->path) - 1);
                        strncpy(pl->name, infos[i].name, sizeof(pl->name) - 1);
                        pl->track_count = infos[i].track_count;
                        pl->is_root = M3U_isRootPlaylist(infos[i].path);

                        Metadata_copyNormalized(pl->name_norm, sizeof(pl->name_norm), infos[i].name);
                        M3U_getPlaylistParentFolder(infos[i].path, pl->parent_folder, sizeof(pl->parent_folder));
                        Metadata_copyNormalized(pl->parent_norm, sizeof(pl->parent_norm), pl->parent_folder);

                        playlist_count++;
                    }
                }
            }
            free(infos);
        }
    }

    set_status("Building index...");
    token_count = 0;
    for (int i = 0; i < track_count; i++) index_track_tokens(i);
    for (int i = 0; i < playlist_count; i++) index_playlist_tokens(i);

    set_status("Saving index...");
    strncpy(saved_fingerprint, fp_str, sizeof(saved_fingerprint) - 1);
    save_json_index(fp_str);

    char done[128];
    snprintf(done, sizeof(done), "Ready (%d tracks)", track_count);
    set_status(done);
}

static void* build_thread_func(void* arg) {
    bool force_rebuild = ((intptr_t)arg) != 0;
    PWR_pinToCores(CPU_CORE_EFFICIENCY);

    M3U_init();

    LibraryFingerprint fp;
    compute_fingerprint(&fp);
    char fp_str[64];
    fingerprint_to_string(&fp, fp_str, sizeof(fp_str));

    if (force_rebuild) {
        unlink(INDEX_PATH);
        free_index_data();
        rebuild_index(fp_str);
    } else {
        set_status("Loading index...");
        if (!load_json_index(fp_str)) {
            rebuild_index(fp_str);
        } else {
            char done[128];
            snprintf(done, sizeof(done), "Ready (%d tracks)", track_count);
            set_status(done);
        }
    }

    pthread_mutex_lock(&index_mutex);
    index_ready = true;
    build_running = false;
    pthread_mutex_unlock(&index_mutex);
    return NULL;
}

static bool start_build_thread(bool force_rebuild) {
    pthread_mutex_lock(&index_mutex);
    if (build_running) {
        pthread_mutex_unlock(&index_mutex);
        return false;
    }
    build_running = true;
    index_ready = false;
    pthread_mutex_unlock(&index_mutex);

    pthread_t thread;
    if (pthread_create(&thread, NULL, build_thread_func, (void*)(intptr_t)(force_rebuild ? 1 : 0)) != 0) {
        pthread_mutex_lock(&index_mutex);
        build_running = false;
        pthread_mutex_unlock(&index_mutex);
        snprintf(build_status, sizeof(build_status), "Index build failed");
        return false;
    }
    pthread_detach(thread);
    return true;
}

void LibraryIndex_init(void) {
    if (build_started) return;
    build_started = true;
    snprintf(build_status, sizeof(build_status), "Starting...");
    start_build_thread(false);
}

bool LibraryIndex_requestRebuild(void) {
    set_status("Rebuilding index...");
    return start_build_thread(true);
}

void LibraryIndex_quit(void) {
    free_index_data();
    index_ready = false;
    build_started = false;
}

bool LibraryIndex_isReady(void) {
    pthread_mutex_lock(&index_mutex);
    bool ready = index_ready;
    pthread_mutex_unlock(&index_mutex);
    return ready;
}

bool LibraryIndex_isBuilding(void) {
    pthread_mutex_lock(&index_mutex);
    bool running = build_running;
    pthread_mutex_unlock(&index_mutex);
    return running;
}

const char* LibraryIndex_getBuildStatus(void) {
    return build_status;
}

typedef struct {
    bool* track_hit;
    bool* playlist_hit;
} HitCtx;

static void mark_hits_cb(const char* token, void* userdata) {
    HitCtx* ctx = (HitCtx*)userdata;
    int ti = find_token(token);
    if (ti < 0) return;

    IndexToken* t = &tokens[ti];
    for (int i = 0; i < t->track_count; i++) {
        int id = t->track_ids[i];
        if (id >= 0 && id < track_count) ctx->track_hit[id] = true;
    }
    for (int i = 0; i < t->playlist_count; i++) {
        int id = t->playlist_ids[i];
        if (id >= 0 && id < playlist_count) ctx->playlist_hit[id] = true;
    }
}

static int score_track(int id, const char query_tokens[][48], int query_token_count) {
    if (id < 0 || id >= track_count) return 0;
    IndexedTrack* tr = &tracks[id];
    int score = 0;

    for (int q = 0; q < query_token_count; q++) {
        const char* qt = query_tokens[q];
        char norm[256];

        Metadata_copyNormalized(norm, sizeof(norm), tr->title);
        if (strstr(norm, qt)) score += 10;
        Metadata_copyNormalized(norm, sizeof(norm), tr->artist);
        if (strstr(norm, qt)) score += 8;
        Metadata_copyNormalized(norm, sizeof(norm), tr->album);
        if (strstr(norm, qt)) score += 6;
        Metadata_copyNormalized(norm, sizeof(norm), tr->genre);
        if (strstr(norm, qt)) score += 4;
        if (strstr(tr->filename_norm, qt)) score += 2;
    }
    return score;
}

static int score_nested_playlist(int id, const char query_tokens[][48], int query_token_count) {
    if (id < 0 || id >= playlist_count) return 0;
    IndexedPlaylist* pl = &playlists[id];
    int score = 0;

    for (int q = 0; q < query_token_count; q++) {
        const char* qt = query_tokens[q];
        if (strstr(pl->name_norm, qt)) score += 10;
        if (strstr(pl->parent_norm, qt)) score += 6;
    }
    return score;
}

static int score_root_playlist(int id, const char query_tokens[][48], int query_token_count) {
    if (id < 0 || id >= playlist_count) return 0;
    IndexedPlaylist* pl = &playlists[id];
    int score = 0;

    for (int q = 0; q < query_token_count; q++) {
        if (strstr(pl->name_norm, query_tokens[q])) score += 10;
    }
    return score;
}

typedef struct {
    int id;
    int score;
    SearchResultType type;
} ScoredItem;

static int compare_scored(const void* a, const void* b) {
    const ScoredItem* sa = (const ScoredItem*)a;
    const ScoredItem* sb = (const ScoredItem*)b;
    if (sb->score != sa->score) return sb->score - sa->score;
    return sa->id - sb->id;
}

static void fill_track_row(SearchResultRow* row, int id, int score) {
    IndexedTrack* tr = &tracks[id];
    row->type = SEARCH_ITEM_TRACK;
    row->score = score;
    strncpy(row->path, tr->path, sizeof(row->path) - 1);
    strncpy(row->label, tr->title[0] ? tr->title : tr->filename_norm, sizeof(row->label) - 1);

    if (tr->artist[0] && tr->album[0]) {
        snprintf(row->subtitle, sizeof(row->subtitle), "%s — %s", tr->artist, tr->album);
    } else if (tr->artist[0]) {
        snprintf(row->subtitle, sizeof(row->subtitle), "%s", tr->artist);
    } else if (tr->album[0]) {
        snprintf(row->subtitle, sizeof(row->subtitle), "%s", tr->album);
    } else {
        row->subtitle[0] = '\0';
    }
    row->track_count = 1;
    row->format = tr->format;
}

static void fill_playlist_row(SearchResultRow* row, int id, int score, bool nested) {
    IndexedPlaylist* pl = &playlists[id];
    row->type = nested ? SEARCH_ITEM_NESTED_PLAYLIST : SEARCH_ITEM_USER_PLAYLIST;
    row->score = score;
    strncpy(row->path, pl->path, sizeof(row->path) - 1);
    row->track_count = pl->track_count;
    row->format = AUDIO_FORMAT_UNKNOWN;

    if (nested && pl->parent_folder[0]) {
        snprintf(row->label, sizeof(row->label), "%s / %s", pl->parent_folder, pl->name);
        snprintf(row->subtitle, sizeof(row->subtitle), "%d tracks", pl->track_count);
    } else {
        snprintf(row->label, sizeof(row->label), "%s", pl->name);
        snprintf(row->subtitle, sizeof(row->subtitle), "%d tracks", pl->track_count);
    }
}

typedef struct {
    char (*tokens)[48];
    int count;
} QueryCollectCtx;

static void collect_query_token(const char* token, void* userdata) {
    QueryCollectCtx* ctx = (QueryCollectCtx*)userdata;
    if (ctx->count >= 32) return;
    strncpy(ctx->tokens[ctx->count], token, 47);
    ctx->tokens[ctx->count][47] = '\0';
    ctx->count++;
}

bool LibraryIndex_search(const char* query, SearchResults* out) {
    if (!out || !query) return false;
    memset(out, 0, sizeof(*out));

    if (!LibraryIndex_isReady()) return false;
    if (track_count == 0 && playlist_count == 0) return false;

    char query_tokens[32][48];
    QueryCollectCtx qctx = {.tokens = query_tokens, .count = 0};

    Metadata_foreachToken(query, collect_query_token, &qctx);
    int query_token_count = qctx.count;
    if (query_token_count == 0) return false;

    bool* track_hit = calloc(track_count, sizeof(bool));
    bool* playlist_hit = calloc(playlist_count, sizeof(bool));
    if (!track_hit || !playlist_hit) {
        free(track_hit);
        free(playlist_hit);
        return false;
    }

    HitCtx hctx = {track_hit, playlist_hit};
    for (int q = 0; q < query_token_count; q++) {
        int ti = find_token(query_tokens[q]);
        if (ti >= 0) {
            IndexToken* t = &tokens[ti];
            for (int i = 0; i < t->track_count; i++) {
                int id = t->track_ids[i];
                if (id >= 0 && id < track_count) track_hit[id] = true;
            }
            for (int i = 0; i < t->playlist_count; i++) {
                int id = t->playlist_ids[i];
                if (id >= 0 && id < playlist_count) playlist_hit[id] = true;
            }
        } else {
            Metadata_foreachToken(query_tokens[q], mark_hits_cb, &hctx);
        }
    }

    ScoredItem* nested_items = calloc(playlist_count, sizeof(ScoredItem));
    ScoredItem* mixed_items = calloc(track_count + playlist_count, sizeof(ScoredItem));
    int nested_n = 0;
    int mixed_n = 0;

    if (!nested_items || !mixed_items) {
        free(track_hit);
        free(playlist_hit);
        free(nested_items);
        free(mixed_items);
        return false;
    }

    for (int i = 0; i < playlist_count; i++) {
        if (!playlist_hit[i]) continue;
        if (playlists[i].is_root) continue;

        int sc = score_nested_playlist(i, query_tokens, query_token_count);
        if (sc <= 0) sc = 1;
        nested_items[nested_n++] = (ScoredItem){
            .id = i, .score = sc, .type = SEARCH_ITEM_NESTED_PLAYLIST
        };
    }
    qsort(nested_items, nested_n, sizeof(ScoredItem), compare_scored);

    int nested_take = nested_n < LIBRARY_SEARCH_MAX_TOP ? nested_n : LIBRARY_SEARCH_MAX_TOP;
    out->nested_count = nested_take;
    out->mixed_limit = LIBRARY_SEARCH_BASE_MIXED + (LIBRARY_SEARCH_MAX_TOP - nested_take);

    for (int i = 0; i < nested_take; i++) {
        fill_playlist_row(&out->nested[i], nested_items[i].id, nested_items[i].score, true);
    }

    for (int i = 0; i < track_count; i++) {
        if (!track_hit[i]) continue;
        int sc = score_track(i, query_tokens, query_token_count);
        if (sc <= 0) sc = 1;
        mixed_items[mixed_n++] = (ScoredItem){
            .id = i, .score = sc, .type = SEARCH_ITEM_TRACK
        };
    }
    for (int i = 0; i < playlist_count; i++) {
        if (!playlist_hit[i]) continue;
        if (!playlists[i].is_root) continue;

        int sc = score_root_playlist(i, query_tokens, query_token_count);
        if (sc <= 0) sc = 1;
        mixed_items[mixed_n++] = (ScoredItem){
            .id = i, .score = sc, .type = SEARCH_ITEM_USER_PLAYLIST
        };
    }
    qsort(mixed_items, mixed_n, sizeof(ScoredItem), compare_scored);

    int mixed_take = mixed_n < out->mixed_limit ? mixed_n : out->mixed_limit;
    out->mixed_count = mixed_take;

    for (int i = 0; i < mixed_take; i++) {
        if (mixed_items[i].type == SEARCH_ITEM_TRACK) {
            fill_track_row(&out->mixed[i], mixed_items[i].id, mixed_items[i].score);
        } else {
            fill_playlist_row(&out->mixed[i], mixed_items[i].id, mixed_items[i].score, false);
        }
    }

    free(track_hit);
    free(playlist_hit);
    free(nested_items);
    free(mixed_items);
    return out->nested_count > 0 || out->mixed_count > 0;
}
