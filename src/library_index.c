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

#define LIBRARY_INDEX_FORMAT_VERSION 4
#define MUSIC_PATH SDCARD_PATH "/Music"
#define INDEX_BIN_PATH SHARED_USERDATA_PATH "/music-player/library_index.bin"
#define INDEX_JSON_PATH SHARED_USERDATA_PATH "/music-player/library_index.json"
#define LIBRARY_MAX_TRACKS 32768
#define LIBRARY_MAX_TOKENS 131072
#define TOKEN_GROW 16
#define TOKEN_HASH_BUCKETS 16384
#define FUZZY_MIN_TOKEN_LEN 3
#define FUZZY_RESULT_THRESHOLD 3
#define INDEX_BIN_MAGIC 0x49504D4E

typedef struct {
    char path[512];
    char title[256];
    char artist[256];
    char album[256];
    char genre[256];
    char title_norm[256];
    char artist_norm[256];
    char album_norm[256];
    char genre_norm[256];
    char filename_norm[256];
    AudioFormat format;
    uint64_t file_mtime;
    uint64_t file_size;
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
    uint64_t hash;
} LibraryFingerprint;

typedef struct {
    char path[512];
    uint64_t mtime;
    uint64_t size;
    bool is_playlist;
} FpEntry;

typedef struct TokenHashNode {
    int token_idx;
    struct TokenHashNode* next;
} TokenHashNode;

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t fingerprint;
    uint32_t track_count;
    uint32_t playlist_count;
} IndexBinHeader;

typedef struct {
    char path[512];
    char title[256];
    char artist[256];
    char album[256];
    char genre[256];
    char filename_norm[256];
    uint32_t format;
    uint64_t mtime;
    uint64_t size;
} IndexBinTrack;

typedef struct {
    char path[512];
    char name[128];
    char name_norm[128];
    char parent_folder[64];
    char parent_norm[64];
    int32_t track_count;
    uint8_t is_root;
    uint8_t pad[3];
} IndexBinPlaylist;
#pragma pack(pop)

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

static TokenHashNode* token_hash[TOKEN_HASH_BUCKETS] = {0};
static TokenHashNode* token_hash_nodes = NULL;
static int token_hash_node_count = 0;
static int token_hash_node_cap = 0;

static uint64_t saved_fingerprint = 0;

static void set_status(const char* msg) {
    pthread_mutex_lock(&index_mutex);
    snprintf(build_status, sizeof(build_status), "%s", msg ? msg : "");
    pthread_mutex_unlock(&index_mutex);
}

static void set_index_ready(bool ready) {
    pthread_mutex_lock(&index_mutex);
    index_ready = ready;
    pthread_mutex_unlock(&index_mutex);
}

static uint64_t fnv1a_update(uint64_t h, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static uint32_t hash_token_string(const char* s) {
    return (uint32_t)fnv1a_update(14695981039346656037ULL, s, strlen(s));
}

static int fp_entry_cmp(const void* a, const void* b) {
    return strcmp(((const FpEntry*)a)->path, ((const FpEntry*)b)->path);
}

static bool skip_dirent_name(const char* name, bool include_hidden) {
    if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
        return true;
    }
    return !include_hidden && name[0] == '.';
}

static void fp_collect_dir(const char* dir, FpEntry** entries, int* count, int* cap, bool playlists_only) {
    DIR* d = opendir(dir);
    if (!d) return;

    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (skip_dirent_name(ent->d_name, true)) continue;

        char full[768];
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);

        struct stat st;
        if (lstat(full, &st) != 0) continue;
        if (S_ISLNK(st.st_mode)) continue;

        if (S_ISDIR(st.st_mode)) {
            fp_collect_dir(full, entries, count, cap, playlists_only);
            continue;
        }

        if (Playlist_isIndexSidecar(ent->d_name)) continue;

        bool include = false;
        if (playlists_only) {
            size_t len = strlen(ent->d_name);
            include = (len > 4 && strcasecmp(ent->d_name + len - 4, ".m3u") == 0);
        } else {
            include = Browser_isAudioFile(ent->d_name);
        }
        if (!include) continue;

        if (*count >= *cap) {
            int nc = *cap ? *cap * 2 : 4096;
            FpEntry* ne = realloc(*entries, sizeof(FpEntry) * nc);
            if (!ne) continue;
            *entries = ne;
            *cap = nc;
        }

        FpEntry* e = &(*entries)[*count];
        strncpy(e->path, full, sizeof(e->path) - 1);
        e->mtime = (uint64_t)st.st_mtime;
        e->size = (uint64_t)st.st_size;
        e->is_playlist = playlists_only;
        (*count)++;
    }
    closedir(d);
}

static void compute_fingerprint(LibraryFingerprint* fp) {
    FpEntry* entries = NULL;
    int count = 0;
    int cap = 0;

    fp_collect_dir(MUSIC_PATH, &entries, &count, &cap, false);
    fp_collect_dir(PLAYLISTS_DIR, &entries, &count, &cap, true);

    if (count > 1) {
        qsort(entries, (size_t)count, sizeof(FpEntry), fp_entry_cmp);
    }

    uint64_t h = 14695981039346656037ULL;
    for (int i = 0; i < count; i++) {
        h = fnv1a_update(h, entries[i].path, strlen(entries[i].path));
        h = fnv1a_update(h, &entries[i].mtime, sizeof(entries[i].mtime));
        h = fnv1a_update(h, &entries[i].size, sizeof(entries[i].size));
    }
    free(entries);

    fp->hash = h;
}

static void fingerprint_to_string(const LibraryFingerprint* fp, char* out, int out_size) {
    snprintf(out, out_size, "v%d_%016llx",
             LIBRARY_INDEX_FORMAT_VERSION,
             (unsigned long long)fp->hash);
}

static void token_hash_clear(void) {
    memset(token_hash, 0, sizeof(token_hash));
    free(token_hash_nodes);
    token_hash_nodes = NULL;
    token_hash_node_count = 0;
    token_hash_node_cap = 0;
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

    token_hash_clear();
}

static void token_hash_clear(void) {
    memset(token_hash, 0, sizeof(token_hash));
    free(token_hash_nodes);
    token_hash_nodes = NULL;
    token_hash_node_count = 0;
    token_hash_node_cap = 0;
}

static void token_hash_insert(int token_idx) {
    if (token_idx < 0 || token_idx >= token_count) return;

    if (token_hash_node_count >= token_hash_node_cap) {
        int nc = token_hash_node_cap ? token_hash_node_cap * 2 : 4096;
        TokenHashNode* nn = realloc(token_hash_nodes, sizeof(TokenHashNode) * nc);
        if (!nn) return;
        token_hash_nodes = nn;
        token_hash_node_cap = nc;
    }

    TokenHashNode* node = &token_hash_nodes[token_hash_node_count++];
    node->token_idx = token_idx;
    node->next = NULL;

    uint32_t bucket = hash_token_string(tokens[token_idx].token) % TOKEN_HASH_BUCKETS;
    node->next = token_hash[bucket];
    token_hash[bucket] = node;
}

static int find_token(const char* token) {
    if (!token || !token[0]) return -1;

    uint32_t bucket = hash_token_string(token) % TOKEN_HASH_BUCKETS;
    for (TokenHashNode* n = token_hash[bucket]; n; n = n->next) {
        if (strcmp(tokens[n->token_idx].token, token) == 0) {
            return n->token_idx;
        }
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
    token_hash_insert(idx);
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

static void add_fuzzy_keys(const char* token, int id, bool is_track) {
    if (!token || strlen(token) < FUZZY_MIN_TOKEN_LEN) return;

    size_t len = strlen(token);
    char key[48];

    for (size_t p = FUZZY_MIN_TOKEN_LEN; p < len; p++) {
        memcpy(key, token, p);
        key[p] = '\0';
        int ti = ensure_token(key);
        if (ti >= 0) {
            if (is_track) token_add_track(ti, id);
            else token_add_playlist(ti, id);
        }
    }

    for (size_t t = 0; t + FUZZY_MIN_TOKEN_LEN <= len; t++) {
        memcpy(key, token + t, FUZZY_MIN_TOKEN_LEN);
        key[FUZZY_MIN_TOKEN_LEN] = '\0';
        int ti = ensure_token(key);
        if (ti >= 0) {
            if (is_track) token_add_track(ti, id);
            else token_add_playlist(ti, id);
        }
    }
}

static void add_token_cb(const char* token, void* userdata) {
    TokenAddCtx* ctx = (TokenAddCtx*)userdata;
    int ti = ensure_token(token);
    if (ti < 0) return;
    if (ctx->is_track) token_add_track(ti, ctx->id);
    else token_add_playlist(ti, ctx->id);
    add_fuzzy_keys(token, ctx->id, ctx->is_track);
}

static void index_track_norms(IndexedTrack* tr) {
    if (!tr) return;
    Metadata_copyNormalized(tr->title_norm, sizeof(tr->title_norm), tr->title);
    Metadata_copyNormalized(tr->artist_norm, sizeof(tr->artist_norm), tr->artist);
    Metadata_copyNormalized(tr->album_norm, sizeof(tr->album_norm), tr->album);
    Metadata_copyNormalized(tr->genre_norm, sizeof(tr->genre_norm), tr->genre);
}

static void index_phrase_token(const char* norm_text, int track_id) {
    if (!norm_text || norm_text[0] == '\0') return;
    size_t len = strlen(norm_text);
    if (len < 2 || len >= sizeof(tokens[0].token)) return;

    int ti = ensure_token(norm_text);
    if (ti >= 0) token_add_track(ti, track_id);
}

static void index_track_tokens(int id) {
    if (id < 0 || id >= track_count) return;
    IndexedTrack* tr = &tracks[id];
    index_track_norms(tr);

    TokenAddCtx ctx = {.id = id, .is_track = true};

    Metadata_foreachToken(tr->title, add_token_cb, &ctx);
    Metadata_foreachToken(tr->artist, add_token_cb, &ctx);
    Metadata_foreachToken(tr->album, add_token_cb, &ctx);
    Metadata_foreachToken(tr->genre, add_token_cb, &ctx);
    Metadata_foreachToken(tr->filename_norm, add_token_cb, &ctx);

    Metadata_foreachNormalizedToken(tr->title_norm, add_token_cb, &ctx);
    Metadata_foreachNormalizedToken(tr->artist_norm, add_token_cb, &ctx);
    Metadata_foreachNormalizedToken(tr->album_norm, add_token_cb, &ctx);
    Metadata_foreachNormalizedToken(tr->genre_norm, add_token_cb, &ctx);

    index_phrase_token(tr->title_norm, id);
    index_phrase_token(tr->artist_norm, id);
    index_phrase_token(tr->album_norm, id);
    index_phrase_token(tr->genre_norm, id);

    char combined[512];
    snprintf(combined, sizeof(combined), "%s %s %s %s",
             tr->title_norm, tr->artist_norm, tr->album_norm, tr->genre_norm);
    Metadata_foreachNormalizedToken(combined, add_token_cb, &ctx);
}

static void index_playlist_tokens(int id);

static void clear_token_index(void) {
    for (int i = 0; i < token_count; i++) {
        free(tokens[i].track_ids);
        free(tokens[i].playlist_ids);
    }
    free(tokens);
    tokens = NULL;
    token_count = 0;
    token_cap = 0;
    token_hash_clear();
}

static void rebuild_token_index(void) {
    clear_token_index();
    for (int i = 0; i < track_count; i++) index_track_tokens(i);
    for (int i = 0; i < playlist_count; i++) index_playlist_tokens(i);
}

typedef struct {
    IndexedTrack track;
} TrackCacheEntry;

static int track_cache_cmp(const void* a, const void* b) {
    return strcmp(((const TrackCacheEntry*)a)->track.path,
                  ((const TrackCacheEntry*)b)->track.path);
}

static void copy_track_fields(IndexedTrack* dst, const IndexedTrack* src) {
    if (!dst || !src) return;
    *dst = *src;
}

static void copy_track_from_bin(const IndexBinTrack* src, IndexedTrack* dst) {
    if (!src || !dst) return;
    strncpy(dst->path, src->path, sizeof(dst->path) - 1);
    strncpy(dst->title, src->title, sizeof(dst->title) - 1);
    strncpy(dst->artist, src->artist, sizeof(dst->artist) - 1);
    strncpy(dst->album, src->album, sizeof(dst->album) - 1);
    strncpy(dst->genre, src->genre, sizeof(dst->genre) - 1);
    strncpy(dst->filename_norm, src->filename_norm, sizeof(dst->filename_norm) - 1);
    dst->format = (AudioFormat)src->format;
    dst->file_mtime = src->mtime;
    dst->file_size = src->size;
    index_track_norms(dst);
}

static void copy_playlist_from_bin(const IndexBinPlaylist* src, IndexedPlaylist* dst) {
    if (!src || !dst) return;
    strncpy(dst->path, src->path, sizeof(dst->path) - 1);
    strncpy(dst->name, src->name, sizeof(dst->name) - 1);
    strncpy(dst->name_norm, src->name_norm, sizeof(dst->name_norm) - 1);
    strncpy(dst->parent_folder, src->parent_folder, sizeof(dst->parent_folder) - 1);
    strncpy(dst->parent_norm, src->parent_norm, sizeof(dst->parent_norm) - 1);
    dst->track_count = src->track_count;
    dst->is_root = src->is_root != 0;
}

static uint64_t parse_fingerprint_string(const char* s) {
    if (!s || !s[0]) return 0;
    int version = 0;
    unsigned long long value = 0;
    if (sscanf(s, "v%d_%llx", &version, &value) == 2 &&
        version == LIBRARY_INDEX_FORMAT_VERSION) {
        return (uint64_t)value;
    }
    return 0;
}

static void load_track_from_json_object(JSON_Object* to, IndexedTrack* tr);

static TrackCacheEntry* load_track_cache_from_bin(int* out_count) {
    if (out_count) *out_count = 0;

    FILE* f = fopen(INDEX_BIN_PATH, "rb");
    if (!f) return NULL;

    IndexBinHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 ||
        hdr.magic != INDEX_BIN_MAGIC ||
        hdr.version != LIBRARY_INDEX_FORMAT_VERSION) {
        fclose(f);
        return NULL;
    }

    if (hdr.track_count == 0 || hdr.track_count > LIBRARY_MAX_TRACKS) {
        fclose(f);
        return NULL;
    }

    TrackCacheEntry* cache = calloc(hdr.track_count, sizeof(TrackCacheEntry));
    if (!cache) {
        fclose(f);
        return NULL;
    }

    int count = 0;
    for (uint32_t i = 0; i < hdr.track_count; i++) {
        IndexBinTrack bt;
        if (fread(&bt, sizeof(bt), 1, f) != 1) break;
        copy_track_from_bin(&bt, &cache[count].track);
        count++;
    }

    fseek(f, (long)(sizeof(IndexBinPlaylist) * hdr.playlist_count), SEEK_CUR);
    fclose(f);

    if (count <= 0) {
        free(cache);
        return NULL;
    }

    if (count > 1) {
        qsort(cache, (size_t)count, sizeof(TrackCacheEntry), track_cache_cmp);
    }

    if (out_count) *out_count = count;
    return cache;
}

static TrackCacheEntry* load_track_cache_from_json(int* out_count) {
    if (out_count) *out_count = 0;

    JSON_Value* root = json_parse_file(INDEX_JSON_PATH);
    if (!root) return NULL;

    JSON_Object* obj = json_value_get_object(root);
    JSON_Array* track_arr = obj ? json_object_get_array(obj, "tracks") : NULL;
    if (!track_arr) {
        json_value_free(root);
        return NULL;
    }

    size_t n = json_array_get_count(track_arr);
    if (n == 0) {
        json_value_free(root);
        return NULL;
    }

    TrackCacheEntry* cache = calloc(n, sizeof(TrackCacheEntry));
    if (!cache) {
        json_value_free(root);
        return NULL;
    }

    int count = 0;
    for (size_t i = 0; i < n; i++) {
        JSON_Object* to = json_array_get_object(track_arr, i);
        if (!to) continue;
        load_track_from_json_object(to, &cache[count].track);
        if (!cache[count].track.path[0]) continue;
        count++;
    }

    json_value_free(root);

    if (count <= 0) {
        free(cache);
        return NULL;
    }

    if (count > 1) {
        qsort(cache, (size_t)count, sizeof(TrackCacheEntry), track_cache_cmp);
    }

    if (out_count) *out_count = count;
    return cache;
}

static TrackCacheEntry* load_track_cache(int* out_count) {
    TrackCacheEntry* cache = load_track_cache_from_bin(out_count);
    if (cache) return cache;
    return load_track_cache_from_json(out_count);
}

static bool load_bin_index(uint64_t expected_fp) {
    FILE* f = fopen(INDEX_BIN_PATH, "rb");
    if (!f) return false;

    IndexBinHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 ||
        hdr.magic != INDEX_BIN_MAGIC ||
        hdr.version != LIBRARY_INDEX_FORMAT_VERSION ||
        hdr.fingerprint != expected_fp) {
        fclose(f);
        return false;
    }

    free_index_data();

    if (hdr.track_count > 0) {
        if (hdr.track_count > LIBRARY_MAX_TRACKS) {
            fclose(f);
            return false;
        }
        tracks = calloc(hdr.track_count, sizeof(IndexedTrack));
        if (!tracks) {
            fclose(f);
            return false;
        }
        for (uint32_t i = 0; i < hdr.track_count; i++) {
            IndexBinTrack bt;
            if (fread(&bt, sizeof(bt), 1, f) != 1) {
                free_index_data();
                fclose(f);
                return false;
            }
            copy_track_from_bin(&bt, &tracks[track_count++]);
        }
    }

    if (hdr.playlist_count > 0) {
        playlists = calloc(hdr.playlist_count, sizeof(IndexedPlaylist));
        if (!playlists) {
            free_index_data();
            fclose(f);
            return false;
        }
        for (uint32_t i = 0; i < hdr.playlist_count; i++) {
            IndexBinPlaylist bp;
            if (fread(&bp, sizeof(bp), 1, f) != 1) {
                free_index_data();
                fclose(f);
                return false;
            }
            copy_playlist_from_bin(&bp, &playlists[playlist_count++]);
        }
    }

    fclose(f);
    rebuild_token_index();
    saved_fingerprint = expected_fp;
    return true;
}

static int save_bin_index(uint64_t fp) {
    mkdir(SHARED_USERDATA_PATH "/music-player", 0755);

    FILE* f = fopen(INDEX_BIN_PATH, "wb");
    if (!f) return -1;

    IndexBinHeader hdr = {
        .magic = INDEX_BIN_MAGIC,
        .version = LIBRARY_INDEX_FORMAT_VERSION,
        .fingerprint = fp,
        .track_count = (uint32_t)track_count,
        .playlist_count = (uint32_t)playlist_count
    };

    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        return -1;
    }

    for (int i = 0; i < track_count; i++) {
        IndexBinTrack bt = {0};
        strncpy(bt.path, tracks[i].path, sizeof(bt.path) - 1);
        strncpy(bt.title, tracks[i].title, sizeof(bt.title) - 1);
        strncpy(bt.artist, tracks[i].artist, sizeof(bt.artist) - 1);
        strncpy(bt.album, tracks[i].album, sizeof(bt.album) - 1);
        strncpy(bt.genre, tracks[i].genre, sizeof(bt.genre) - 1);
        strncpy(bt.filename_norm, tracks[i].filename_norm, sizeof(bt.filename_norm) - 1);
        bt.format = (uint32_t)tracks[i].format;
        bt.mtime = tracks[i].file_mtime;
        bt.size = tracks[i].file_size;
        if (fwrite(&bt, sizeof(bt), 1, f) != 1) {
            fclose(f);
            return -1;
        }
    }

    for (int i = 0; i < playlist_count; i++) {
        IndexBinPlaylist bp = {0};
        strncpy(bp.path, playlists[i].path, sizeof(bp.path) - 1);
        strncpy(bp.name, playlists[i].name, sizeof(bp.name) - 1);
        strncpy(bp.name_norm, playlists[i].name_norm, sizeof(bp.name_norm) - 1);
        strncpy(bp.parent_folder, playlists[i].parent_folder, sizeof(bp.parent_folder) - 1);
        strncpy(bp.parent_norm, playlists[i].parent_norm, sizeof(bp.parent_norm) - 1);
        bp.track_count = playlists[i].track_count;
        bp.is_root = playlists[i].is_root ? 1 : 0;
        if (fwrite(&bp, sizeof(bp), 1, f) != 1) {
            fclose(f);
            return -1;
        }
    }

    fclose(f);
    return 0;
}

static bool load_json_index(uint64_t expected_fp) {
    JSON_Value* root = json_parse_file(INDEX_JSON_PATH);
    if (!root) return false;

    JSON_Object* obj = json_value_get_object(root);
    const char* fp = json_object_get_string(obj, "fingerprint");
    if (parse_fingerprint_string(fp) != expected_fp) {
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
            load_track_from_json_object(to, &tracks[track_count]);
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

    rebuild_token_index();
    saved_fingerprint = expected_fp;
    json_value_free(root);

    if (save_bin_index(expected_fp) == 0) {
        unlink(INDEX_JSON_PATH);
    }
    return true;
}

static const TrackCacheEntry* track_cache_find(TrackCacheEntry* cache, int count, const char* path) {
    if (!cache || count <= 0 || !path) return NULL;

    TrackCacheEntry key = {{0}};
    strncpy(key.track.path, path, sizeof(key.track.path) - 1);
    return bsearch(&key, cache, (size_t)count, sizeof(TrackCacheEntry), track_cache_cmp);
}

static void load_track_from_json_object(JSON_Object* to, IndexedTrack* tr) {
    const char* path = json_object_get_string(to, "path");
    if (path) strncpy(tr->path, path, sizeof(tr->path) - 1);
    const char* title = json_object_get_string(to, "title");
    if (title) strncpy(tr->title, title, sizeof(tr->title) - 1);
    const char* artist = json_object_get_string(to, "artist");
    if (artist) strncpy(tr->artist, artist, sizeof(tr->artist) - 1);
    const char* album = json_object_get_string(to, "album");
    if (album) strncpy(tr->album, album, sizeof(tr->album) - 1);
    const char* genre = json_object_get_string(to, "genre");
    if (genre) strncpy(tr->genre, genre, sizeof(tr->genre) - 1);
    const char* fn = json_object_get_string(to, "filename_norm");
    if (fn) strncpy(tr->filename_norm, fn, sizeof(tr->filename_norm) - 1);
    tr->format = (AudioFormat)json_object_get_number(to, "format");
    tr->file_mtime = (uint64_t)json_object_get_number(to, "mtime");
    tr->file_size = (uint64_t)json_object_get_number(to, "size");
    index_track_norms(tr);
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

static void rebuild_index(uint64_t fp) {
    free_index_data();

    int cache_count = 0;
    TrackCacheEntry* cache = load_track_cache(&cache_count);

    set_status("Scanning music...");
    {
        char** paths = NULL;
        int count = Playlist_collectPathsEx(MUSIC_PATH, &paths, LIBRARY_MAX_TRACKS, true);
        if (count > 0 && paths) {
            tracks = calloc(count, sizeof(IndexedTrack));
            if (tracks) {
                char status[128];
                for (int i = 0; i < count; i++) {
                    if ((i & 63) == 0) {
                        snprintf(status, sizeof(status), "Reading tags... %d/%d", i, count);
                        set_status(status);
                    }

                    struct stat st;
                    if (stat(paths[i], &st) != 0) continue;

                    IndexedTrack* tr = &tracks[track_count];
                    const TrackCacheEntry* cached = track_cache_find(cache, cache_count, paths[i]);
                    if (cached && cached->track.file_mtime != 0 &&
                        cached->track.file_mtime == (uint64_t)st.st_mtime &&
                        cached->track.file_size == (uint64_t)st.st_size) {
                        copy_track_fields(tr, &cached->track);
                    } else {
                        strncpy(tr->path, paths[i], sizeof(tr->path) - 1);
                        tr->format = Player_detectFormat(paths[i]);

                        TrackMetadata meta;
                        Metadata_readFromFileEx(paths[i], tr->format, &meta);
                        strncpy(tr->title, meta.title, sizeof(tr->title) - 1);
                        strncpy(tr->artist, meta.artist, sizeof(tr->artist) - 1);
                        strncpy(tr->album, meta.album, sizeof(tr->album) - 1);
                        strncpy(tr->genre, meta.genre, sizeof(tr->genre) - 1);

                        const char* base = strrchr(paths[i], '/');
                        base = base ? base + 1 : paths[i];
                        Metadata_copyNormalized(tr->filename_norm, sizeof(tr->filename_norm), base);
                        index_track_norms(tr);

                        tr->file_mtime = (uint64_t)st.st_mtime;
                        tr->file_size = (uint64_t)st.st_size;
                    }

                    track_count++;
                }
                snprintf(status, sizeof(status), "Reading tags... %d/%d", count, count);
                set_status(status);
            }
            Playlist_freePaths(paths, count);
        }
    }
    free(cache);

    set_status("Building track index...");
    rebuild_token_index();
    set_index_ready(true);

    char partial[128];
    snprintf(partial, sizeof(partial), "Ready tracks (%d), scanning playlists...", track_count);
    set_status(partial);

    set_status("Scanning playlists...");
    {
        int limit = Settings_getMaxPlaylists();
        PlaylistInfo* infos = calloc(limit, sizeof(PlaylistInfo));
        if (infos) {
            int n = M3U_listAllPlaylistsEx(infos, limit, Settings_getPlaylistScanDepth(), true);
            if (n > 0) {
                free(playlists);
                playlists = calloc(n, sizeof(IndexedPlaylist));
                playlist_count = 0;
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
    rebuild_token_index();

    set_status("Saving index...");
    saved_fingerprint = fp;
    save_bin_index(fp);

    char done[128];
    snprintf(done, sizeof(done), "Ready (%d tracks)", track_count);
    set_status(done);
}

static void* build_thread_func(void* arg) {
    bool force_rebuild = ((intptr_t)arg) != 0;
    PWR_pinToCores(CPU_CORE_PERFORMANCE);

    M3U_init();

    LibraryFingerprint fp;
    compute_fingerprint(&fp);

    if (force_rebuild) {
        rebuild_index(fp.hash);
    } else {
        set_status("Loading index...");
        if (load_bin_index(fp.hash)) {
            char done[128];
            snprintf(done, sizeof(done), "Ready (%d tracks)", track_count);
            set_status(done);
        } else if (load_json_index(fp.hash)) {
            char done[128];
            snprintf(done, sizeof(done), "Ready (%d tracks)", track_count);
            set_status(done);
        } else {
            rebuild_index(fp.hash);
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

static int gather_query_keys(const char* token, char keys[][48], int max_keys, bool fuzzy) {
    if (!token || !token[0] || max_keys <= 0) return 0;

    int count = 0;
    strncpy(keys[count], token, 47);
    keys[count][47] = '\0';
    count++;

    if (!fuzzy || strlen(token) < FUZZY_MIN_TOKEN_LEN) return count;

    size_t len = strlen(token);
    for (size_t p = FUZZY_MIN_TOKEN_LEN; p < len && count < max_keys; p++) {
        bool dup = false;
        for (int i = 0; i < count; i++) {
            if (strncmp(keys[i], token, p) == 0 && keys[i][p] == '\0') {
                dup = true;
                break;
            }
        }
        if (!dup) {
            memcpy(keys[count], token, p);
            keys[count][p] = '\0';
            count++;
        }
    }

    for (size_t t = 0; t + FUZZY_MIN_TOKEN_LEN <= len && count < max_keys; t++) {
        char tri[4];
        memcpy(tri, token + t, FUZZY_MIN_TOKEN_LEN);
        tri[FUZZY_MIN_TOKEN_LEN] = '\0';
        bool dup = false;
        for (int i = 0; i < count; i++) {
            if (strcmp(keys[i], tri) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            strncpy(keys[count], tri, 47);
            keys[count][47] = '\0';
            count++;
        }
    }

    return count;
}

static void count_token_postings(const char query_tokens[][48], int query_token_count, bool fuzzy,
                                 int* track_hits, int* playlist_hits) {
    bool* track_matched_token = calloc((size_t)track_count, sizeof(bool));
    bool* playlist_matched_token = calloc((size_t)playlist_count, sizeof(bool));
    if (!track_matched_token || !playlist_matched_token) {
        free(track_matched_token);
        free(playlist_matched_token);
        return;
    }

    for (int q = 0; q < query_token_count; q++) {
        char keys[24][48];
        int key_count = gather_query_keys(query_tokens[q], keys, 24, fuzzy);
        memset(track_matched_token, 0, (size_t)track_count * sizeof(bool));
        memset(playlist_matched_token, 0, (size_t)playlist_count * sizeof(bool));

        for (int k = 0; k < key_count; k++) {
            int ti = find_token(keys[k]);
            if (ti < 0) continue;

            IndexToken* tok = &tokens[ti];
            for (int i = 0; i < tok->track_count; i++) {
                int id = tok->track_ids[i];
                if (id >= 0 && id < track_count && !track_matched_token[id]) {
                    track_matched_token[id] = true;
                    track_hits[id]++;
                }
            }
            for (int i = 0; i < tok->playlist_count; i++) {
                int id = tok->playlist_ids[i];
                if (id >= 0 && id < playlist_count && !playlist_matched_token[id]) {
                    playlist_matched_token[id] = true;
                    playlist_hits[id]++;
                }
            }
        }
    }

    free(track_matched_token);
    free(playlist_matched_token);
}

static bool track_field_contains_token(const char* field, const char* token) {
    return field && field[0] && token && token[0] && strstr(field, token) != NULL;
}

static int score_track(int id, const char query_tokens[][48], int query_token_count) {
    if (id < 0 || id >= track_count) return 0;
    IndexedTrack* tr = &tracks[id];
    int score = 0;

    for (int q = 0; q < query_token_count; q++) {
        const char* qt = query_tokens[q];
        if (track_field_contains_token(tr->title_norm, qt)) score += 10;
        if (track_field_contains_token(tr->artist_norm, qt)) score += 8;
        if (track_field_contains_token(tr->album_norm, qt)) score += 6;
        if (track_field_contains_token(tr->genre_norm, qt)) score += 4;
        if (track_field_contains_token(tr->filename_norm, qt)) score += 2;
    }
    return score;
}

static int score_track_with_phrase(int id, const char query_tokens[][48], int query_token_count,
                                   const char* query_phrase) {
    int score = score_track(id, query_tokens, query_token_count);
    if (!query_phrase || !query_phrase[0] || id < 0 || id >= track_count) return score;

    IndexedTrack* tr = &tracks[id];
    if (track_field_contains_token(tr->title_norm, query_phrase)) score += 15;
    if (track_field_contains_token(tr->artist_norm, query_phrase)) score += 12;
    if (track_field_contains_token(tr->album_norm, query_phrase)) score += 10;
    return score;
}

static bool track_matches_all_tokens(int id, const char query_tokens[][48], int query_token_count) {
    if (id < 0 || id >= track_count || query_token_count <= 0) return false;

    IndexedTrack* tr = &tracks[id];
    const char* fields[] = {
        tr->title_norm, tr->artist_norm, tr->album_norm, tr->genre_norm, tr->filename_norm
    };

    for (int q = 0; q < query_token_count; q++) {
        const char* qt = query_tokens[q];
        bool found = false;
        for (int f = 0; f < 5; f++) {
            if (track_field_contains_token(fields[f], qt)) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

static bool track_matches_query(int id, const char query_tokens[][48], int query_token_count,
                                const char* query_phrase) {
    if (id < 0 || id >= track_count) return false;

    if (query_phrase && strlen(query_phrase) >= 3) {
        IndexedTrack* tr = &tracks[id];
        if (track_field_contains_token(tr->album_norm, query_phrase)) return true;
        if (track_field_contains_token(tr->artist_norm, query_phrase)) return true;
        if (track_field_contains_token(tr->title_norm, query_phrase)) return true;
    }

    return track_matches_all_tokens(id, query_tokens, query_token_count);
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

    pthread_mutex_lock(&index_mutex);
    if (track_count == 0 && playlist_count == 0) {
        pthread_mutex_unlock(&index_mutex);
        return false;
    }

    char query_tokens[32][48];
    QueryCollectCtx qctx = {.tokens = query_tokens, .count = 0};

    Metadata_foreachToken(query, collect_query_token, &qctx);
    int query_token_count = qctx.count;
    if (query_token_count == 0) {
        pthread_mutex_unlock(&index_mutex);
        return false;
    }

    char query_phrase[512];
    Metadata_copyNormalized(query_phrase, sizeof(query_phrase), query);

    int* track_hits = calloc((size_t)track_count, sizeof(int));
    int* playlist_hits = calloc((size_t)playlist_count, sizeof(int));
    bool* track_hit = calloc((size_t)track_count, sizeof(bool));
    bool* playlist_hit = calloc((size_t)playlist_count, sizeof(bool));
    if (!track_hits || !playlist_hits || !track_hit || !playlist_hit) {
        free(track_hits);
        free(playlist_hits);
        free(track_hit);
        free(playlist_hit);
        pthread_mutex_unlock(&index_mutex);
        return false;
    }

    count_token_postings(query_tokens, query_token_count, false, track_hits, playlist_hits);

    bool fuzzy_enabled = Settings_getFuzzySearch();
    int exact_track_hits = 0;
    for (int i = 0; i < track_count; i++) {
        if (track_hits[i] < query_token_count) continue;
        if (track_matches_query(i, query_tokens, query_token_count, query_phrase)) {
            track_hit[i] = true;
            exact_track_hits++;
        }
    }

    for (int i = 0; i < playlist_count; i++) {
        if (playlist_hits[i] >= query_token_count) {
            playlist_hit[i] = true;
        }
    }

    if (fuzzy_enabled && exact_track_hits < FUZZY_RESULT_THRESHOLD) {
        memset(track_hits, 0, (size_t)track_count * sizeof(int));
        memset(playlist_hits, 0, (size_t)playlist_count * sizeof(int));
        count_token_postings(query_tokens, query_token_count, true, track_hits, playlist_hits);

        for (int i = 0; i < track_count; i++) {
            if (track_hit[i]) continue;
            if (track_hits[i] >= query_token_count) {
                track_hit[i] = true;
            }
        }

        for (int i = 0; i < playlist_count; i++) {
            if (playlist_hits[i] >= query_token_count) {
                playlist_hit[i] = true;
            }
        }
    }

    ScoredItem* nested_items = calloc((size_t)playlist_count, sizeof(ScoredItem));
    ScoredItem* mixed_items = calloc((size_t)(track_count + playlist_count), sizeof(ScoredItem));
    int nested_n = 0;
    int mixed_n = 0;

    if (!nested_items || !mixed_items) {
        free(track_hits);
        free(playlist_hits);
        free(track_hit);
        free(playlist_hit);
        free(nested_items);
        free(mixed_items);
        pthread_mutex_unlock(&index_mutex);
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
        int sc = score_track_with_phrase(i, query_tokens, query_token_count, query_phrase);
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

    bool has_results = out->nested_count > 0 || out->mixed_count > 0;

    free(track_hits);
    free(playlist_hits);
    free(track_hit);
    free(playlist_hit);
    free(nested_items);
    free(mixed_items);
    pthread_mutex_unlock(&index_mutex);
    return has_results;
}
