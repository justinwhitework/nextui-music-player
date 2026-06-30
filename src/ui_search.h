#ifndef __UI_SEARCH_H__
#define __UI_SEARCH_H__

#include <SDL2/SDL.h>
#include "library_index.h"
#include "playlist.h"

void render_search_building(SDL_Surface* screen, int show_setting, const char* status);

void render_search_query(SDL_Surface* screen, int show_setting, const char* query);

void render_search_results(SDL_Surface* screen, int show_setting,
                           const char* query,
                           const SearchResults* results,
                           int selected, int scroll);

void render_search_detail(SDL_Surface* screen, int show_setting,
                          const char* title,
                          const char* m3u_path,
                          PlaylistTrack* tracks, int count,
                          int selected, int scroll);

int search_results_total_count(const SearchResults* results);

bool search_result_at(const SearchResults* results, int flat_index, SearchResultRow* out_row);

#endif
