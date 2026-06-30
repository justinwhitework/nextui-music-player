#include "track_art.h"
#include "image_utils.h"
#include "m4a_metadata.h"
#include "player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include "audio/dr_flac.h"

#define TRACK_ART_CACHE_SIZE 64
#define TRACK_ART_QUEUE_SIZE 32
#define TRACK_ART_THUMB_SIZE_DEFAULT 48

typedef struct {
    char path[512];
    SDL_Surface* thumb;
    int thumb_size;
    bool loaded;
    bool no_art;
} TrackArtCacheEntry;

typedef struct {
    char path[512];
} TrackArtQueueEntry;

static TrackArtCacheEntry track_art_cache[TRACK_ART_CACHE_SIZE];
static int track_art_cache_count = 0;

static TrackArtQueueEntry track_art_queue[TRACK_ART_QUEUE_SIZE];
static int track_art_queue_count = 0;
static int track_art_queue_head = 0;

#define FILE_ART_CACHE_SIZE 16

typedef struct {
    char path[512];
    SDL_Surface* thumb;
    int thumb_size;
} FileArtCacheEntry;

static FileArtCacheEntry file_art_cache[FILE_ART_CACHE_SIZE];
static int file_art_cache_count = 0;

static uint32_t read_syncsafe_int(const uint8_t* data) {
    return ((uint32_t)(data[0] & 0x7F) << 21) |
           ((uint32_t)(data[1] & 0x7F) << 14) |
           ((uint32_t)(data[2] & 0x7F) << 7) |
           ((uint32_t)(data[3] & 0x7F));
}

static uint32_t read_be32(const uint8_t* data) {
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

static SDL_Surface* load_art_from_memory(const uint8_t* image_data, size_t image_size) {
    if (!image_data || image_size == 0) return NULL;
    SDL_RWops* rw = SDL_RWFromConstMem(image_data, image_size);
    if (!rw) return NULL;
    return IMG_Load_RW(rw, 1);
}

static SDL_Surface* extract_mp3_art(const char* filepath) {
    FILE* f = fopen(filepath, "rb");
    if (!f) return NULL;

    uint8_t header[10];
    if (fread(header, 1, 10, f) != 10) {
        fclose(f);
        return NULL;
    }

    if (header[0] != 'I' || header[1] != 'D' || header[2] != '3') {
        fclose(f);
        return NULL;
    }

    uint8_t version_major = header[3];
    uint32_t tag_size = read_syncsafe_int(&header[6]);

    uint8_t* tag_data = malloc(tag_size);
    if (!tag_data) {
        fclose(f);
        return NULL;
    }

    if (fread(tag_data, 1, tag_size, f) != tag_size) {
        free(tag_data);
        fclose(f);
        return NULL;
    }
    fclose(f);

    SDL_Surface* art = NULL;
    uint32_t pos = 0;
    while (pos + 10 < tag_size && !art) {
        char frame_id[5];
        memcpy(frame_id, &tag_data[pos], 4);
        frame_id[4] = '\0';
        if (frame_id[0] == '\0') break;

        uint32_t frame_size;
        if (version_major == 4) {
            frame_size = read_syncsafe_int(&tag_data[pos + 4]);
        } else {
            frame_size = read_be32(&tag_data[pos + 4]);
        }

        pos += 10;
        if (frame_size == 0 || pos + frame_size > tag_size) break;

        if (strcmp(frame_id, "APIC") == 0 && frame_size > 10) {
            const uint8_t* frame_data = &tag_data[pos];
            uint8_t encoding = frame_data[0];
            size_t offset = 1;

            while (offset < frame_size && frame_data[offset] != '\0') offset++;
            offset++;

            if (offset < frame_size) {
                uint8_t pic_type = frame_data[offset++];
                (void)pic_type;

                if (encoding == 1 || encoding == 2) {
                    while (offset + 1 < frame_size) {
                        if (frame_data[offset] == 0 && frame_data[offset + 1] == 0) {
                            offset += 2;
                            break;
                        }
                        offset++;
                    }
                } else {
                    while (offset < frame_size && frame_data[offset] != '\0') offset++;
                    offset++;
                }

                if (offset < frame_size) {
                    size_t image_size = frame_size - offset;
                    art = load_art_from_memory(&frame_data[offset], image_size);
                }
            }
        }

        pos += frame_size;
    }

    free(tag_data);
    return art;
}

typedef struct {
    SDL_Surface* art;
} FlacArtContext;

static void flac_art_callback(void* pUserData, drflac_metadata* pMetadata) {
    FlacArtContext* ctx = (FlacArtContext*)pUserData;
    if (!ctx || ctx->art) return;

    if (pMetadata->type == DRFLAC_METADATA_BLOCK_TYPE_PICTURE) {
        const drflac_allocation_callbacks* callbacks = NULL;
        (void)callbacks;
        if (pMetadata->data.picture.pPictureData && pMetadata->data.picture.pictureDataSize > 0) {
            ctx->art = load_art_from_memory(
                pMetadata->data.picture.pPictureData,
                pMetadata->data.picture.pictureDataSize);
        }
    }
}

static SDL_Surface* extract_flac_art(const char* filepath) {
    FlacArtContext ctx = {0};
    drflac* flac = drflac_open_file_with_metadata(filepath, flac_art_callback, &ctx, NULL);
    if (flac) {
        drflac_close(flac);
    }
    return ctx.art;
}

static SDL_Surface* extract_track_art(const char* filepath) {
    AudioFormat format = Player_detectFormat(filepath);
    switch (format) {
        case AUDIO_FORMAT_MP3:
            return extract_mp3_art(filepath);
        case AUDIO_FORMAT_M4A:
        case AUDIO_FORMAT_AAC:
            return M4A_extractCoverArt(filepath);
        case AUDIO_FORMAT_FLAC:
            return extract_flac_art(filepath);
        default:
            return NULL;
    }
}

static TrackArtCacheEntry* find_cache_entry(const char* filepath) {
    for (int i = 0; i < track_art_cache_count; i++) {
        if (strcmp(track_art_cache[i].path, filepath) == 0) {
            return &track_art_cache[i];
        }
    }
    return NULL;
}

static void evict_cache_index(int index) {
    if (track_art_cache[index].thumb) {
        SDL_FreeSurface(track_art_cache[index].thumb);
    }
    for (int i = index; i < track_art_cache_count - 1; i++) {
        track_art_cache[i] = track_art_cache[i + 1];
    }
    track_art_cache_count--;
}

static TrackArtCacheEntry* ensure_cache_entry(const char* filepath) {
    TrackArtCacheEntry* existing = find_cache_entry(filepath);
    if (existing) return existing;

    if (track_art_cache_count >= TRACK_ART_CACHE_SIZE) {
        evict_cache_index(0);
    }

    TrackArtCacheEntry* entry = &track_art_cache[track_art_cache_count++];
    memset(entry, 0, sizeof(*entry));
    strncpy(entry->path, filepath, sizeof(entry->path) - 1);
    return entry;
}

static bool queue_contains(const char* filepath) {
    for (int i = 0; i < track_art_queue_count; i++) {
        int idx = (track_art_queue_head + i) % TRACK_ART_QUEUE_SIZE;
        if (strcmp(track_art_queue[idx].path, filepath) == 0) {
            return true;
        }
    }
    return false;
}

void TrackArt_init(void) {
    track_art_cache_count = 0;
    track_art_queue_count = 0;
    track_art_queue_head = 0;
}

void TrackArt_cleanup(void) {
    TrackArt_clearCache();
    track_art_queue_count = 0;
    track_art_queue_head = 0;
}

void TrackArt_clearCache(void) {
    while (track_art_cache_count > 0) {
        evict_cache_index(0);
    }
    track_art_queue_count = 0;
    track_art_queue_head = 0;

    for (int i = 0; i < file_art_cache_count; i++) {
        if (file_art_cache[i].thumb) {
            SDL_FreeSurface(file_art_cache[i].thumb);
        }
    }
    file_art_cache_count = 0;
}

void TrackArt_request(const char* filepath) {
    if (!filepath || !filepath[0]) return;

    TrackArtCacheEntry* entry = find_cache_entry(filepath);
    if (entry && (entry->loaded || entry->no_art)) return;
    if (queue_contains(filepath)) return;

    if (track_art_queue_count >= TRACK_ART_QUEUE_SIZE) return;

    int tail = (track_art_queue_head + track_art_queue_count) % TRACK_ART_QUEUE_SIZE;
    strncpy(track_art_queue[tail].path, filepath, sizeof(track_art_queue[tail].path) - 1);
    track_art_queue_count++;
}

bool TrackArt_hasPendingWork(void) {
    return track_art_queue_count > 0;
}

void TrackArt_tick(void) {
    if (track_art_queue_count == 0) return;

    TrackArtQueueEntry job = track_art_queue[track_art_queue_head];
    track_art_queue_head = (track_art_queue_head + 1) % TRACK_ART_QUEUE_SIZE;
    track_art_queue_count--;

    TrackArtCacheEntry* entry = ensure_cache_entry(job.path);
    if (!entry || entry->loaded || entry->no_art) return;

    SDL_Surface* raw = extract_track_art(job.path);
    if (raw) {
        entry->thumb = image_scale_to_square(raw, TRACK_ART_THUMB_SIZE_DEFAULT);
        SDL_FreeSurface(raw);
        entry->thumb_size = TRACK_ART_THUMB_SIZE_DEFAULT;
        entry->loaded = (entry->thumb != NULL);
        entry->no_art = (entry->thumb == NULL);
    } else {
        entry->loaded = false;
        entry->no_art = true;
    }
}

SDL_Surface* TrackArt_getThumbnail(const char* filepath, int size) {
    if (!filepath || !filepath[0] || size <= 0) return NULL;

    TrackArtCacheEntry* entry = find_cache_entry(filepath);
    if (!entry || !entry->thumb) return NULL;

    if (entry->thumb_size == size) {
        return entry->thumb;
    }

    SDL_Surface* scaled = image_scale_to_square(entry->thumb, size);
    if (!scaled) return entry->thumb;

    SDL_FreeSurface(entry->thumb);
    entry->thumb = scaled;
    entry->thumb_size = size;
    return entry->thumb;
}

static const char* const k_sidecar_exts[] = {".png", ".webp", ".jpg", ".jpeg", NULL};

bool TrackArt_findSidecarPath(const char* filepath, char* out, int out_size) {
    if (!filepath || !out || out_size <= 0) return false;

    char base[512];
    strncpy(base, filepath, sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';

    char* dot = strrchr(base, '.');
    if (dot) *dot = '\0';

    for (int i = 0; k_sidecar_exts[i]; i++) {
        snprintf(out, out_size, "%s%s", base, k_sidecar_exts[i]);
        if (access(out, F_OK) == 0) return true;
    }
    return false;
}

static const char* const k_dir_art_names[] = {
    "folder.png", "folder.webp", "Folder.png", "Folder.webp",
    "cover.png", "cover.webp", "Cover.png", "Cover.webp",
    "album.png", "album.webp", "Album.png", "Album.webp",
    NULL
};

bool TrackArt_findDirArtPath(const char* dir, char* out, int out_size) {
    if (!dir || !dir[0] || !out || out_size <= 0) return false;

    for (int i = 0; k_dir_art_names[i]; i++) {
        snprintf(out, out_size, "%s/%s", dir, k_dir_art_names[i]);
        if (access(out, F_OK) == 0) return true;
    }
    return false;
}

static SDL_Surface* load_image_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0 || fsize > 2 * 1024 * 1024) {
        fclose(f);
        return NULL;
    }

    uint8_t* data = (uint8_t*)malloc((size_t)fsize);
    if (!data) {
        fclose(f);
        return NULL;
    }

    if (fread(data, 1, (size_t)fsize, f) != (size_t)fsize) {
        free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);

    SDL_RWops* rw = SDL_RWFromConstMem(data, fsize);
    SDL_Surface* raw = NULL;
    if (rw) raw = IMG_Load_RW(rw, 1);
    free(data);
    return raw;
}

static SDL_Surface* get_file_art_thumbnail(const char* image_path, int size) {
    if (!image_path || !image_path[0] || size <= 0) return NULL;

    for (int i = 0; i < file_art_cache_count; i++) {
        if (strcmp(file_art_cache[i].path, image_path) == 0) {
            if (file_art_cache[i].thumb && file_art_cache[i].thumb_size == size) {
                return file_art_cache[i].thumb;
            }
            if (file_art_cache[i].thumb) {
                SDL_FreeSurface(file_art_cache[i].thumb);
                file_art_cache[i].thumb = NULL;
            }
            SDL_Surface* raw = load_image_file(image_path);
            if (raw) {
                file_art_cache[i].thumb = image_scale_to_square(raw, size);
                SDL_FreeSurface(raw);
            }
            file_art_cache[i].thumb_size = size;
            return file_art_cache[i].thumb;
        }
    }

    SDL_Surface* raw = load_image_file(image_path);
    if (!raw) return NULL;

    SDL_Surface* thumb = image_scale_to_square(raw, size);
    SDL_FreeSurface(raw);
    if (!thumb) return NULL;

    if (file_art_cache_count >= FILE_ART_CACHE_SIZE) {
        SDL_FreeSurface(file_art_cache[0].thumb);
        for (int i = 1; i < file_art_cache_count; i++) {
            file_art_cache[i - 1] = file_art_cache[i];
        }
        file_art_cache_count--;
    }

    FileArtCacheEntry* slot = &file_art_cache[file_art_cache_count++];
    memset(slot, 0, sizeof(*slot));
    strncpy(slot->path, image_path, sizeof(slot->path) - 1);
    slot->thumb = thumb;
    slot->thumb_size = size;
    return thumb;
}

SDL_Surface* TrackArt_getHistoryThumbnail(const char* item_path, const char* art_dir, int size) {
    if (!item_path || !item_path[0] || size <= 0) return NULL;

    TrackArt_request(item_path);
    SDL_Surface* thumb = TrackArt_getThumbnail(item_path, size);
    if (thumb) return thumb;

    char sidecar[512];
    if (TrackArt_findSidecarPath(item_path, sidecar, sizeof(sidecar))) {
        thumb = get_file_art_thumbnail(sidecar, size);
        if (thumb) return thumb;
    }

    if (art_dir && art_dir[0]) {
        char dir_art[512];
        if (TrackArt_findDirArtPath(art_dir, dir_art, sizeof(dir_art))) {
            return get_file_art_thumbnail(dir_art, size);
        }
    }

    return NULL;
}
