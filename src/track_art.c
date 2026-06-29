#include "track_art.h"
#include "image_utils.h"
#include "m4a_metadata.h"
#include "player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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
