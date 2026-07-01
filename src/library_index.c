#define _GNU_SOURCE
#include "library_index.h"
#include "metadata_reader.h"
#include "playlist_m3u.h"
#include "playlist.h"
#include "player.h"
#include "settings.h"
#include "browser.h"
#include "search_fuse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>

#include "defines.h"
#include "api.h"

#define MUSIC_PATH SDCARD_PATH "/Music"
#define INDEX_DIR SHARED_USERDATA_PATH "/music-player/index"
#define INDEX_META INDEX_DIR "/index.meta"
#define SONGS_TSV INDEX_DIR "/songs.index.tsv"
#define PLAYLISTS_TSV INDEX_DIR "/playlists.index.tsv"
#define SONGS_TMP INDEX_DIR "/songs.index.tsv.tmp"
#define PLAYLISTS_TMP INDEX_DIR "/playlists.index.tsv.tmp"
#define META_TMP INDEX_DIR "/index.meta.tmp"
#define LEGACY_JSON SHARED_USERDATA_PATH "/music-player/library_index.json"

#define TSV_LINE_MAX 1408
#define ABORT_CHECK_EVERY 256

static SearchResultRow search_nested_heap[LIBRARY_SEARCH_MAX_TOP];
static SearchResultRow search_mixed_heap[LIBRARY_SEARCH_MAX_MIXED];

typedef struct {
    uint64_t file_count;
    uint64_t latest_mtime;
} LibraryFingerprint;

typedef struct {
    char (*tokens)[SEARCH_FUSE_TOKEN_MAX];
    int count;
} QueryCollectCtx;

static pthread_mutex_t index_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t search_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool build_started = false;
static bool build_running = false;
static bool index_ready = false;
static char build_status[128] = "Starting...";
static int cached_song_count = 0;
static int cached_playlist_count = 0;

static volatile bool search_abort = false;
static volatile bool search_timed_out = false;

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

static bool ensure_index_dir(void) {
    mkdir(SHARED_USERDATA_PATH "/music-player", 0755);
    return mkdir(INDEX_DIR, 0755) == 0 || errno == EEXIST;
}

static void sanitize_tsv_field(const char* src, char* dst, size_t dst_size) {
    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < dst_size; i++) {
        char c = src[i];
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        dst[j++] = c;
    }
    dst[j] = '\0';
}

static void unlink_tmp_files(void) {
    unlink(SONGS_TMP);
    unlink(PLAYLISTS_TMP);
    unlink(META_TMP);
}

static void delete_index_files(void) {
    unlink(SONGS_TSV);
    unlink(PLAYLISTS_TSV);
    unlink(INDEX_META);
    unlink(LEGACY_JSON);
    unlink_tmp_files();
}

static bool file_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

typedef struct {
    int version;
    char fingerprint[64];
    int song_count;
    int playlist_count;
    int songs_truncated;
    int playlists_truncated;
} IndexMeta;

static bool parse_meta_file(const char* path, IndexMeta* meta) {
    memset(meta, 0, sizeof(*meta));
    FILE* f = fopen(path, "r");
    if (!f) return false;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "version=%d", &meta->version) == 1) continue;
        if (sscanf(line, "fingerprint=%63s", meta->fingerprint) == 1) continue;
        if (sscanf(line, "song_count=%d", &meta->song_count) == 1) continue;
        if (sscanf(line, "playlist_count=%d", &meta->playlist_count) == 1) continue;
        if (sscanf(line, "songs_truncated=%d", &meta->songs_truncated) == 1) continue;
        if (sscanf(line, "playlists_truncated=%d", &meta->playlists_truncated) == 1) continue;
    }
    fclose(f);
    if (meta->song_count < 0) meta->song_count = 0;
    if (meta->playlist_count < 0) meta->playlist_count = 0;
    if (meta->song_count > INDEX_MAX_ROWS) meta->song_count = INDEX_MAX_ROWS;
    if (meta->playlist_count > INDEX_MAX_ROWS) meta->playlist_count = INDEX_MAX_ROWS;
    return meta->version == 1 && meta->fingerprint[0];
}

static bool index_cache_valid(const char* expected_fp) {
    if (!file_exists(INDEX_META) || !file_exists(SONGS_TSV) || !file_exists(PLAYLISTS_TSV)) {
        return false;
    }
    IndexMeta meta;
    if (!parse_meta_file(INDEX_META, &meta)) return false;
    if (strcmp(meta.fingerprint, expected_fp) != 0) return false;
    cached_song_count = meta.song_count;
    cached_playlist_count = meta.playlist_count;
    return true;
}

static bool write_meta_tmp(const char* fp_str, int song_count, int playlist_count,
                           int songs_truncated, int playlists_truncated) {
    FILE* f = fopen(META_TMP, "w");
    if (!f) return false;
    if (song_count < 0) song_count = 0;
    if (playlist_count < 0) playlist_count = 0;
    if (song_count > INDEX_MAX_ROWS) song_count = INDEX_MAX_ROWS;
    if (playlist_count > INDEX_MAX_ROWS) playlist_count = INDEX_MAX_ROWS;
    fprintf(f, "version=1\n");
    fprintf(f, "fingerprint=%s\n", fp_str);
    fprintf(f, "song_count=%d\n", song_count);
    fprintf(f, "playlist_count=%d\n", playlist_count);
    fprintf(f, "songs_truncated=%d\n", songs_truncated);
    fprintf(f, "playlists_truncated=%d\n", playlists_truncated);
    fprintf(f, "built_at=%ld\n", (long)time(NULL));
    fclose(f);
    return true;
}

static bool commit_index_files(void) {
    if (rename(SONGS_TMP, SONGS_TSV) != 0) return false;
    if (rename(PLAYLISTS_TMP, PLAYLISTS_TSV) != 0) {
        unlink(SONGS_TSV);
        return false;
    }
    if (rename(META_TMP, INDEX_META) != 0) {
        unlink(SONGS_TSV);
        unlink(PLAYLISTS_TSV);
        return false;
    }
    unlink(LEGACY_JSON);
    return true;
}

static bool tsv_line_complete(const char* line, FILE* fp) {
    if (!line || !line[0]) return true;
    size_t len = strlen(line);
    if (len < TSV_LINE_MAX - 1) return true;
    if (line[len - 1] == '\n') return true;
    return feof(fp) != 0;
}

static int parse_tsv_fields(char* line, char** fields, int max_fields) {
    int count = 0;
    char* p = line;
    while (count < max_fields) {
        fields[count++] = p;
        char* tab = strchr(p, '\t');
        if (!tab) break;
        *tab = '\0';
        p = tab + 1;
    }
    if (count > 0) {
        char* nl = strchr(fields[count - 1], '\n');
        if (nl) *nl = '\0';
        nl = strchr(fields[count - 1], '\r');
        if (nl) *nl = '\0';
    }
    return count;
}

static void collect_query_token(const char* token, void* userdata) {
    QueryCollectCtx* ctx = (QueryCollectCtx*)userdata;
    if (!ctx || !token) return;
    if (ctx->count >= 32) return;
    strncpy(ctx->tokens[ctx->count], token, SEARCH_FUSE_TOKEN_MAX - 1);
    ctx->tokens[ctx->count][SEARCH_FUSE_TOKEN_MAX - 1] = '\0';
    ctx->count++;
}

static int compare_row_score(const void* a, const void* b) {
    const SearchResultRow* ra = (const SearchResultRow*)a;
    const SearchResultRow* rb = (const SearchResultRow*)b;
    if (rb->score != ra->score) return rb->score - ra->score;
    return strcmp(ra->path, rb->path);
}

static void topk_insert(SearchResultRow* heap, int* count, int max_count, const SearchResultRow* item) {
    if (!heap || !count || !item || max_count <= 0) return;
    if (*count < 0) *count = 0;
    if (*count > max_count) *count = max_count;
    if (item->score < SEARCH_FUSE_MIN_SCORE) return;

    if (*count < max_count) {
        heap[(*count)++] = *item;
        return;
    }

    int min_i = 0;
    for (int i = 1; i < *count; i++) {
        if (heap[i].score < heap[min_i].score) min_i = i;
    }
    if (item->score > heap[min_i].score) {
        heap[min_i] = *item;
    }
}

static bool search_should_abort(int row_index) {
    if ((row_index & (ABORT_CHECK_EVERY - 1)) != 0) return false;
    return search_abort;
}

static int score_playlist_row(const char* path, const char* title,
                              const char query_norm[256],
                              const char query_tokens[][SEARCH_FUSE_TOKEN_MAX],
                              int query_token_count) {
    if (!path) return 0;

    char name_norm[128];
    char parent_norm[64];
    char file_norm[128];

    Metadata_copyNormalized(name_norm, sizeof(name_norm), title);

    char parent_folder[64];
    M3U_getPlaylistParentFolder(path, parent_folder, sizeof(parent_folder));
    Metadata_copyNormalized(parent_norm, sizeof(parent_norm), parent_folder);

    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    Metadata_copyNormalized(file_norm, sizeof(file_norm), base);

    int score = SearchFuse_scorePlaylist(name_norm, parent_norm, query_norm,
                                         query_tokens, query_token_count, true);
    int path_score = SearchFuse_matchScore(file_norm, query_norm, true);
    if (path_score > score) score = path_score;

    for (int t = 0; t < query_token_count; t++) {
        int ts = SearchFuse_matchScore(file_norm, query_tokens[t], true);
        if (ts > score) score = ts;
    }
    return score;
}

static void fill_song_result(SearchResultRow* row, const char* path,
                             const char* title, const char* album, const char* artist,
                             int score) {
    if (!row || !path) return;
    memset(row, 0, sizeof(*row));
    row->type = SEARCH_ITEM_TRACK;
    row->score = score;
    strncpy(row->path, path, sizeof(row->path) - 1);
    row->path[sizeof(row->path) - 1] = '\0';

    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    strncpy(row->label, (title && title[0]) ? title : base, sizeof(row->label) - 1);
    row->label[sizeof(row->label) - 1] = '\0';

    if (artist && artist[0] && album && album[0]) {
        snprintf(row->subtitle, sizeof(row->subtitle), "%s — %s", artist, album);
    } else if (artist && artist[0]) {
        snprintf(row->subtitle, sizeof(row->subtitle), "%s", artist);
    } else if (album && album[0]) {
        snprintf(row->subtitle, sizeof(row->subtitle), "%s", album);
    }

    row->track_count = 1;
    row->format = Player_detectFormat(path);
}

static void fill_playlist_result(SearchResultRow* row, const char* path, const char* title,
                                 int track_count, bool is_root, int score) {
    if (!row || !path) return;
    memset(row, 0, sizeof(*row));
    row->type = is_root ? SEARCH_ITEM_USER_PLAYLIST : SEARCH_ITEM_NESTED_PLAYLIST;
    row->score = score;
    strncpy(row->path, path, sizeof(row->path) - 1);
    row->path[sizeof(row->path) - 1] = '\0';
    row->track_count = track_count < 0 ? 0 : track_count;
    row->format = AUDIO_FORMAT_UNKNOWN;

    char parent_folder[64];
    M3U_getPlaylistParentFolder(path, parent_folder, sizeof(parent_folder));

    if (!is_root && parent_folder[0]) {
        snprintf(row->label, sizeof(row->label), "%s / %s", parent_folder, title ? title : "");
    } else {
        snprintf(row->label, sizeof(row->label), "%s", title ? title : "");
    }
    snprintf(row->subtitle, sizeof(row->subtitle), "%d tracks", track_count);
}

static bool rebuild_index(const char* fp_str) {
    if (!ensure_index_dir()) return false;
    unlink_tmp_files();

    FILE* songs_fp = fopen(SONGS_TMP, "w");
    if (!songs_fp) return false;

    int songs_written = 0;
    int songs_truncated = 0;

    set_status("Scanning music...");
    {
        char** paths = NULL;
        int collected = Playlist_collectPaths(MUSIC_PATH, &paths, INDEX_MAX_ROWS + 1);
        int write_count = collected;
        if (collected > INDEX_MAX_ROWS) {
            songs_truncated = 1;
            write_count = INDEX_MAX_ROWS;
        }

        for (int i = 0; i < write_count; i++) {
            if (songs_written >= INDEX_MAX_ROWS) {
                songs_truncated = 1;
                break;
            }
            if (!paths || !paths[i]) continue;
            if ((i & 255) == 0) {
                char msg[128];
                snprintf(msg, sizeof(msg), "Indexing songs %d/%d", i + 1, write_count);
                set_status(msg);
            }

            TrackMetadata meta;
            Metadata_readFromFile(paths[i], &meta);

            char path_field[512];
            char title_field[256];
            char album_field[256];
            char artist_field[256];

            sanitize_tsv_field(paths[i], path_field, sizeof(path_field));

            const char* base = strrchr(paths[i], '/');
            base = base ? base + 1 : paths[i];
            char stem[256];
            strncpy(stem, base, sizeof(stem) - 1);
            stem[sizeof(stem) - 1] = '\0';
            char* ext = strrchr(stem, '.');
            if (ext) *ext = '\0';

            const char* title_src = meta.title[0] ? meta.title : stem;
            sanitize_tsv_field(title_src, title_field, sizeof(title_field));
            sanitize_tsv_field(meta.album, album_field, sizeof(album_field));
            sanitize_tsv_field(meta.artist, artist_field, sizeof(artist_field));

            fprintf(songs_fp, "%s\t%s\t%s\t%s\n",
                    path_field, title_field, album_field, artist_field);
            songs_written++;
        }

        if (paths) Playlist_freePaths(paths, collected);
    }

    fclose(songs_fp);

    if (songs_truncated) {
        set_status("Song index full (65536 limit)");
    }

    FILE* pl_fp = fopen(PLAYLISTS_TMP, "w");
    if (!pl_fp) {
        unlink(SONGS_TMP);
        return false;
    }

    int playlists_written = 0;
    int playlists_truncated = 0;

    set_status("Scanning playlists...");
    {
        int limit = Settings_getMaxPlaylists();
        if (limit > INDEX_MAX_ROWS + 1) limit = INDEX_MAX_ROWS + 1;

        PlaylistInfo* infos = calloc(limit, sizeof(PlaylistInfo));
        if (infos) {
            int n = M3U_listAllPlaylists(infos, limit, Settings_getPlaylistScanDepth());
            if (n < 0) n = 0;
            if (n > INDEX_MAX_ROWS) {
                playlists_truncated = 1;
                n = INDEX_MAX_ROWS;
            }

            for (int i = 0; i < n; i++) {
                if (infos[i].is_folder) continue;
                if (playlists_written >= INDEX_MAX_ROWS) {
                    playlists_truncated = 1;
                    break;
                }
                if (!infos[i].path[0]) continue;

                char path_field[512];
                char title_field[256];
                sanitize_tsv_field(infos[i].path, path_field, sizeof(path_field));
                sanitize_tsv_field(infos[i].name, title_field, sizeof(title_field));

                int is_root = M3U_isRootPlaylist(infos[i].path) ? 1 : 0;
                fprintf(pl_fp, "%s\t%s\t%d\t%d\n",
                        path_field, title_field, infos[i].track_count, is_root);
                playlists_written++;
            }
            free(infos);
        }
    }

    fclose(pl_fp);

    if (playlists_truncated) {
        set_status("Playlist index full (65536 limit)");
    }

    set_status("Saving index...");
    if (!write_meta_tmp(fp_str, songs_written, playlists_written,
                        songs_truncated, playlists_truncated)) {
        unlink_tmp_files();
        return false;
    }

    if (!commit_index_files()) {
        unlink_tmp_files();
        return false;
    }

    cached_song_count = songs_written;
    cached_playlist_count = playlists_written;
    return true;
}

static void* build_thread_func(void* arg) {
    bool force_rebuild = ((intptr_t)arg) != 0;
    PWR_pinToCores(CPU_CORE_EFFICIENCY);

    M3U_init();

    LibraryFingerprint fp;
    compute_fingerprint(&fp);
    char fp_str[64];
    fingerprint_to_string(&fp, fp_str, sizeof(fp_str));

    bool ok = false;
    if (force_rebuild) {
        delete_index_files();
    }

    if (!force_rebuild && index_cache_valid(fp_str)) {
        ok = true;
    } else {
        ok = rebuild_index(fp_str);
    }

    char done[128];
    if (ok) {
        snprintf(done, sizeof(done), "Ready (%d songs)", cached_song_count);
    } else {
        snprintf(done, sizeof(done), "Index build failed");
    }
    set_status(done);

    pthread_mutex_lock(&index_mutex);
    index_ready = ok;
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
    LibraryIndex_searchAbort();
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

void LibraryIndex_searchAbort(void) {
    search_abort = true;
}

void LibraryIndex_searchMarkTimedOut(void) {
    search_timed_out = true;
}

void LibraryIndex_searchClearAbort(void) {
    search_abort = false;
    search_timed_out = false;
}

bool LibraryIndex_searchTimedOut(void) {
    return search_timed_out;
}

bool LibraryIndex_search(const char* query, SearchResults* out) {
    if (!out || !query) return false;
    memset(out, 0, sizeof(*out));

    if (!LibraryIndex_isReady()) return false;
    if (!file_exists(SONGS_TSV) && !file_exists(PLAYLISTS_TSV)) return false;

    if (pthread_mutex_lock(&search_mutex) != 0) return false;

    char query_tokens[32][SEARCH_FUSE_TOKEN_MAX];
    QueryCollectCtx qctx = {.tokens = query_tokens, .count = 0};
    Metadata_foreachToken(query, collect_query_token, &qctx);
    if (qctx.count == 0) {
        pthread_mutex_unlock(&search_mutex);
        return false;
    }

    char query_norm[256];
    Metadata_copyNormalized(query_norm, sizeof(query_norm), query);

    SearchResultRow* nested_heap = search_nested_heap;
    SearchResultRow* mixed_heap = search_mixed_heap;
    int nested_n = 0;
    int mixed_n = 0;

    FILE* songs_fp = fopen(SONGS_TSV, "r");
    if (songs_fp) {
        char line[TSV_LINE_MAX];
        int row = 0;
        while (fgets(line, sizeof(line), songs_fp)) {
            if (row >= INDEX_MAX_ROWS) break;
            if (search_should_abort(row)) break;
            if (!tsv_line_complete(line, songs_fp)) {
                row++;
                continue;
            }

            char* fields[4];
            if (parse_tsv_fields(line, fields, 4) < 4) {
                row++;
                continue;
            }

            const char* path = fields[0];
            if (!path[0]) {
                row++;
                continue;
            }
            const char* title = fields[1];
            const char* album = fields[2];
            const char* artist = fields[3];

            char title_norm[256];
            char artist_norm[256];
            char album_norm[256];
            char file_norm[256];

            Metadata_copyNormalized(title_norm, sizeof(title_norm), title);
            Metadata_copyNormalized(artist_norm, sizeof(artist_norm), artist);
            Metadata_copyNormalized(album_norm, sizeof(album_norm), album);

            const char* base = strrchr(path, '/');
            base = base ? base + 1 : path;
            Metadata_copyNormalized(file_norm, sizeof(file_norm), base);

            SearchFuseFields fields_fuse = {
                .title = title_norm,
                .artist = artist_norm,
                .album = album_norm,
                .genre = NULL,
                .filename = file_norm
            };

            int score = SearchFuse_scoreFields(&fields_fuse, query_norm,
                                               query_tokens, qctx.count, true);
            if (score >= SEARCH_FUSE_MIN_SCORE) {
                SearchResultRow row_out;
                fill_song_result(&row_out, path, title, album, artist, score);
                topk_insert(mixed_heap, &mixed_n, LIBRARY_SEARCH_MAX_MIXED, &row_out);
            }
            row++;
        }
        fclose(songs_fp);
    }

    if (!search_abort) {
        FILE* pl_fp = fopen(PLAYLISTS_TSV, "r");
        if (pl_fp) {
            char line[TSV_LINE_MAX];
            int row = 0;
            while (fgets(line, sizeof(line), pl_fp)) {
                if (row >= INDEX_MAX_ROWS) break;
                if (search_should_abort(row)) break;
                if (!tsv_line_complete(line, pl_fp)) {
                    row++;
                    continue;
                }

                char* fields[4];
                if (parse_tsv_fields(line, fields, 4) < 4) {
                    row++;
                    continue;
                }

                const char* path = fields[0];
                if (!path[0]) {
                    row++;
                    continue;
                }
                const char* title = fields[1];
                int track_count = atoi(fields[2]);
                if (track_count < 0) track_count = 0;
                bool is_root = atoi(fields[3]) != 0;

                int score = score_playlist_row(path, title, query_norm, query_tokens, qctx.count);
                if (score >= SEARCH_FUSE_MIN_SCORE) {
                    SearchResultRow row_out;
                    fill_playlist_result(&row_out, path, title, track_count, is_root, score);
                    if (is_root) {
                        topk_insert(mixed_heap, &mixed_n, LIBRARY_SEARCH_MAX_MIXED, &row_out);
                    } else {
                        topk_insert(nested_heap, &nested_n, LIBRARY_SEARCH_MAX_TOP, &row_out);
                    }
                }
                row++;
            }
            fclose(pl_fp);
        }
    }

    if (nested_n > LIBRARY_SEARCH_MAX_TOP) nested_n = LIBRARY_SEARCH_MAX_TOP;
    if (mixed_n > LIBRARY_SEARCH_MAX_MIXED) mixed_n = LIBRARY_SEARCH_MAX_MIXED;

    if (nested_n > 1) qsort(nested_heap, nested_n, sizeof(SearchResultRow), compare_row_score);
    if (mixed_n > 1) qsort(mixed_heap, mixed_n, sizeof(SearchResultRow), compare_row_score);

    int nested_take = nested_n < LIBRARY_SEARCH_MAX_TOP ? nested_n : LIBRARY_SEARCH_MAX_TOP;
    out->nested_count = nested_take;
    out->mixed_limit = LIBRARY_SEARCH_BASE_MIXED + (LIBRARY_SEARCH_MAX_TOP - nested_take);

    for (int i = 0; i < nested_take; i++) {
        out->nested[i] = nested_heap[i];
    }

    int mixed_take = mixed_n < out->mixed_limit ? mixed_n : out->mixed_limit;
    out->mixed_count = mixed_take;
    for (int i = 0; i < mixed_take; i++) {
        out->mixed[i] = mixed_heap[i];
    }

    pthread_mutex_unlock(&search_mutex);
    return out->nested_count > 0 || out->mixed_count > 0;
}
