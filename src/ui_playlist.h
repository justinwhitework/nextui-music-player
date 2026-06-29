#ifndef __UI_PLAYLIST_H__
#define __UI_PLAYLIST_H__

#include <SDL2/SDL.h>
#include "playlist_m3u.h"
#include "playlist.h"

void render_playlist_list(SDL_Surface* screen, int show_setting,
                          const char* dir_title,
                          PlaylistInfo* playlists, int count,
                          int selected, int scroll);

void render_playlist_detail(SDL_Surface* screen, int show_setting,
                            const char* playlist_name,
                            PlaylistTrack* tracks, int count,
                            int selected, int scroll);

bool playlist_list_needs_scroll_refresh(void);
bool playlist_list_scroll_needs_render(void);
void playlist_list_animate_scroll(void);

#endif
