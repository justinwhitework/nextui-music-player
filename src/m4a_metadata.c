#include "m4a_metadata.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#define MINIMP4_IMPLEMENTATION
#include "audio/minimp4.h"

static int m4a_read_callback(int64_t offset, void* buffer, size_t size, void* token) {
    FILE* file = (FILE*)token;
    if (fseek(file, offset, SEEK_SET) != 0) return -1;
    return (fread(buffer, 1, size, file) == size) ? 0 : -1;
}

SDL_Surface* M4A_extractCoverArt(const char* filepath) {
    if (!filepath) return NULL;

    FILE* file = fopen(filepath, "rb");
    if (!file) return NULL;

    fseek(file, 0, SEEK_END);
    int64_t file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    MP4D_demux_t mp4;
    memset(&mp4, 0, sizeof(mp4));
    if (MP4D_open(&mp4, m4a_read_callback, file, file_size) == 0) {
        fclose(file);
        return NULL;
    }

    SDL_Surface* art = NULL;
    if (mp4.tag.cover && mp4.tag.cover_size > 0) {
        SDL_RWops* rw = SDL_RWFromConstMem(mp4.tag.cover, mp4.tag.cover_size);
        if (rw) {
            art = IMG_Load_RW(rw, 1);
        }
    }

    fclose(file);
    return art;
}
