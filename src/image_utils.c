#include "image_utils.h"
#include <SDL2/SDL.h>

bool image_is_complete(const uint8_t* data, int size) {
    if (!data || size < 4) return false;
    if (data[0] == 0xFF && data[1] == 0xD8) {
        return (data[size - 2] == 0xFF && data[size - 1] == 0xD9);
    }
    if (data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47) {
        return (size >= 8 &&
                data[size - 4] == 0xAE && data[size - 3] == 0x42 &&
                data[size - 2] == 0x60 && data[size - 1] == 0x82);
    }
    return true;
}

SDL_Surface* image_scale_to_square(SDL_Surface* src, int size) {
    if (!src || size <= 0) return NULL;

    SDL_Surface* converted = SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_ARGB8888, 0);
    if (!converted) return NULL;

    SDL_Surface* scaled = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!scaled) {
        SDL_FreeSurface(converted);
        return NULL;
    }

    int src_w = converted->w;
    int src_h = converted->h;
    float scale_w = (float)size / src_w;
    float scale_h = (float)size / src_h;
    float scale = (scale_w > scale_h) ? scale_w : scale_h;

    int crop_w = (int)(size / scale);
    int crop_h = (int)(size / scale);
    int crop_x = (src_w - crop_w) / 2;
    int crop_y = (src_h - crop_h) / 2;
    if (crop_x < 0) crop_x = 0;
    if (crop_y < 0) crop_y = 0;
    if (crop_x + crop_w > src_w) crop_w = src_w - crop_x;
    if (crop_y + crop_h > src_h) crop_h = src_h - crop_y;

    SDL_Rect src_rect = {crop_x, crop_y, crop_w, crop_h};
    SDL_Rect dst_rect = {0, 0, size, size};
    SDL_BlitScaled(converted, &src_rect, scaled, &dst_rect);
    SDL_FreeSurface(converted);
    return scaled;
}
