#ifndef __UI_SEARCH_H__
#define __UI_SEARCH_H__

#include <SDL2/SDL.h>
#include <stdbool.h>
#include "library_index.h"
#include "playlist.h"

typedef enum {
    SEARCH_HOME_ITEM_SEARCH = 0,
    SEARCH_HOME_ITEM_HISTORY,
    SEARCH_HOME_ITEM_CLEAR,
    SEARCH_HOME_ITEM_REBUILD
} SearchHomeItemType;

int search_home_item_count(void);
SearchHomeItemType search_home_item_type(int index);
const char* search_home_item_label(int index);

void render_search_building(SDL_Surface* screen, int show_setting, const char* status);

void render_search_home(SDL_Surface* screen, int show_setting, int selected, int scroll);

void render_search_rebuilding(SDL_Surface* screen, int show_setting, const char* status);

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

bool search_results_top_result(const SearchResults* results, SearchResultRow* out_row);

#endif
