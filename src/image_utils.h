#ifndef __IMAGE_UTILS_H__
#define __IMAGE_UTILS_H__

#include <stdbool.h>
#include <stdint.h>

// Validate that image file bytes look complete (JPEG/PNG).
bool image_is_complete(const uint8_t* data, int size);

// Scale surface to size x size (ARGB), caller owns returned surface.
struct SDL_Surface;
struct SDL_Surface* image_scale_to_square(struct SDL_Surface* src, int size);

#endif
