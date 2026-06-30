#include <stdio.h>
#include <string.h>

#include "defines.h"
#include "api.h"
#include "ui_search.h"
#include "ui_fonts.h"
#include "ui_icons.h"
#include "ui_utils.h"
#include "ui_album_art.h"
#include "playlist_art.h"
#include "track_art.h"
#include "settings.h"
#include "search_history.h"
#include "library_index.h"
#include "module_common.h"

static ScrollTextState search_scroll = {0};
static ScrollTextState search_home_scroll = {0};
static ScrollTextState search_detail_scroll = {0};

int search_home_item_count(void) {
    return 3 + SearchHistory_count();
}

SearchHomeItemType search_home_item_type(int index) {
    if (index == 0) return SEARCH_HOME_ITEM_SEARCH;
    int hc = SearchHistory_count();
    if (index >= 1 && index <= hc) return SEARCH_HOME_ITEM_HISTORY;
    if (index == 1 + hc) return SEARCH_HOME_ITEM_CLEAR;
    return SEARCH_HOME_ITEM_REBUILD;
}

const char* search_home_item_label(int index) {
    static char rebuild_label[64];
    SearchHomeItemType type = search_home_item_type(index);

    switch (type) {
        case SEARCH_HOME_ITEM_SEARCH:
            return "Search";
        case SEARCH_HOME_ITEM_HISTORY: {
            int hi = index - 1;
            const char* q = SearchHistory_get(hi);
            return q ? q : "";
        }
        case SEARCH_HOME_ITEM_CLEAR:
            return "Clear history";
        case SEARCH_HOME_ITEM_REBUILD:
            if (LibraryIndex_isBuilding()) {
                snprintf(rebuild_label, sizeof(rebuild_label), "Rebuild Index...");
                return rebuild_label;
            }
            return "Rebuild Index";
        default:
            return "";
    }
}

static SDL_Surface* search_home_icon(int index, bool selected) {
    switch (search_home_item_type(index)) {
        case SEARCH_HOME_ITEM_SEARCH:
            return Icons_getSearch(selected);
        case SEARCH_HOME_ITEM_CLEAR:
            return Icons_getTrash(selected);
        case SEARCH_HOME_ITEM_REBUILD:
            return Icons_getIndex(selected);
        default:
            return NULL;
    }
}

void render_search_building(SDL_Surface* screen, int show_setting, const char* status) {
    GFX_clear(screen);
    render_screen_header(screen, "Library Search", show_setting);

    const char* msg = (status && status[0]) ? status : "Building index...";
    SDL_Surface* text = TTF_RenderUTF8_Blended(Fonts_getMedium(), msg, COLOR_WHITE);
    if (text) {
        SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){
            (screen->w - text->w) / 2,
            screen->h / 2 - text->h / 2
        });
        SDL_FreeSurface(text);
    }

    GFX_blitButtonGroup((char*[]){"START", "CONTROLS", NULL}, 0, screen, 0);
    GFX_blitButtonGroup((char*[]){"B", "BACK", NULL}, 1, screen, 1);
}

void render_search_rebuilding(SDL_Surface* screen, int show_setting, const char* status) {
    GFX_clear(screen);
    render_screen_header(screen, "Rebuild Search Index", show_setting);

    const char* msg = (status && status[0]) ? status : "Rebuilding index...";
    SDL_Surface* text = TTF_RenderUTF8_Blended(Fonts_getMedium(), msg, COLOR_WHITE);
    if (text) {
        SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){
            (screen->w - text->w) / 2,
            screen->h / 2 - text->h / 2
        });
        SDL_FreeSurface(text);
    }

    GFX_blitButtonGroup((char*[]){"START", "CONTROLS", NULL}, 0, screen, 0);
    GFX_blitButtonGroup((char*[]){"B", "BACK", NULL}, 1, screen, 1);
}

void render_search_home(SDL_Surface* screen, int show_setting, int selected, int scroll) {
    GFX_clear(screen);
    render_screen_header(screen, "Library Search", show_setting);

    int total = search_home_item_count();
    ListLayout layout = calc_list_layout(screen);
    bool use_icons = Icons_isLoaded();
    int icon_size = use_icons ? SCALE1(24) : 0;
    int icon_spacing = use_icons ? SCALE1(6) : 0;
    int icon_offset = icon_size + icon_spacing;

    char truncated[256];

    for (int vis = 0; vis < layout.items_per_page && (scroll + vis) < total; vis++) {
        int idx = scroll + vis;
        bool is_selected = (idx == selected);
        int y = layout.list_y + vis * layout.item_h;

        const char* label = search_home_item_label(idx);
        SDL_Surface* icon = use_icons ? search_home_icon(idx, is_selected) : NULL;
        bool use_icon_slot = icon != NULL;
        int row_icon_offset = use_icon_slot ? icon_offset : 0;

        ListItemPos pos = render_list_item_pill(screen, &layout, label, truncated, y, is_selected, row_icon_offset);

        if (use_icon_slot) {
            int icon_y = y + (layout.item_h - icon_size) / 2;
            SDL_Rect src = {0, 0, icon->w, icon->h};
            SDL_Rect dst = {pos.text_x, icon_y, icon_size, icon_size};
            SDL_BlitScaled(icon, &src, screen, &dst);
        }

        int text_x = pos.text_x + (use_icon_slot ? icon_offset : 0);
        int available_width = pos.pill_width - SCALE1(BUTTON_PADDING * 2);
        if (use_icon_slot) available_width -= icon_offset;

        render_list_item_text(screen, &search_home_scroll, label, Fonts_getMedium(),
                              text_x, pos.text_y, available_width, is_selected);
    }

    render_scroll_indicators(screen, scroll, layout.items_per_page, total);
    GFX_blitButtonGroup((char*[]){"START", "CONTROLS", NULL}, 0, screen, 0);
    GFX_blitButtonGroup((char*[]){"B", "BACK", "A", "SELECT", "Y", "SEARCH", NULL}, 1, screen, 1);
}

int search_results_total_count(const SearchResults* results) {
    if (!results) return 0;
    int total = results->mixed_count;
    if (results->nested_count > 0) total += 1 + results->nested_count;
    return total;
}

bool search_result_at(const SearchResults* results, int flat_index, SearchResultRow* out_row) {
    if (!results || !out_row || flat_index < 0) return false;

    if (results->nested_count > 0) {
        if (flat_index == 0) return false;
        flat_index--;
        if (flat_index < results->nested_count) {
            *out_row = results->nested[flat_index];
            return true;
        }
        flat_index -= results->nested_count;
    }

    if (flat_index < results->mixed_count) {
        *out_row = results->mixed[flat_index];
        return true;
    }
    return false;
}

static bool row_is_header(const SearchResults* results, int flat_index) {
    return results && results->nested_count > 0 && flat_index == 0;
}

void render_search_results(SDL_Surface* screen, int show_setting,
                           const char* query,
                           const SearchResults* results,
                           int selected, int scroll) {
    GFX_clear(screen);

    char title[300];
    if (query && query[0]) {
        snprintf(title, sizeof(title), "Search: %s", query);
    } else {
        snprintf(title, sizeof(title), "Search Results");
    }
    render_screen_header(screen, title, show_setting);

    int total = search_results_total_count(results);
    if (total == 0) {
        render_empty_state(screen, "No matches", "Try a different search term", NULL);
        GFX_blitButtonGroup((char*[]){"START", "CONTROLS", NULL}, 0, screen, 0);
        GFX_blitButtonGroup((char*[]){"B", "BACK", "Y", "SEARCH", NULL}, 1, screen, 1);
        return;
    }

    ListLayout layout = calc_list_layout(screen);
    bool tooltip = Settings_getTooltipArtwork();
    int icon_size = Icons_isLoaded() ? SCALE1(24) : 0;
    int icon_spacing = Icons_isLoaded() ? SCALE1(6) : 0;
    int icon_offset = icon_size + icon_spacing;

    char truncated[256];

    for (int i = 0; i < layout.items_per_page && (scroll + i) < total; i++) {
        int idx = scroll + i;
        bool is_selected = (idx == selected);
        int y = layout.list_y + i * layout.item_h;

        if (row_is_header(results, idx)) {
            SDL_Surface* hdr = TTF_RenderUTF8_Blended(Fonts_getSmall(), "Playlists", COLOR_GRAY);
            if (hdr) {
                SDL_BlitSurface(hdr, NULL, screen, &(SDL_Rect){
                    SCALE1(BUTTON_PADDING), y + (layout.item_h - hdr->h) / 2
                });
                SDL_FreeSurface(hdr);
            }
            continue;
        }

        SearchResultRow item;
        if (!search_result_at(results, idx, &item)) continue;

        bool use_icon_slot = Icons_isLoaded() && tooltip;
        int row_icon_offset = use_icon_slot ? icon_offset : 0;

        char display[300];
        if (item.subtitle[0]) {
            snprintf(display, sizeof(display), "%s — %s", item.label, item.subtitle);
        } else {
            snprintf(display, sizeof(display), "%s", item.label);
        }

        ListItemPos pos = render_list_item_pill(screen, &layout, display, truncated, y, is_selected, row_icon_offset);

        if (use_icon_slot) {
            int icon_y = y + (layout.item_h - icon_size) / 2;
            int icon_x = pos.text_x;
            SDL_Surface* thumb = NULL;
            if (item.type == SEARCH_ITEM_TRACK) {
                thumb = TrackArt_getThumbnail(item.path, icon_size);
            } else {
                thumb = PlaylistArt_getThumbnail(item.path, icon_size);
            }
            render_list_icon(screen, icon_x, icon_y, icon_size, thumb,
                             item.type == SEARCH_ITEM_TRACK ? item.format : AUDIO_FORMAT_UNKNOWN,
                             is_selected);
        }

        int text_x = pos.text_x + (use_icon_slot ? icon_offset : 0);
        int available_width = pos.pill_width - SCALE1(BUTTON_PADDING * 2);
        if (use_icon_slot) available_width -= icon_offset;

        render_list_item_text(screen, &search_scroll, display, Fonts_getMedium(),
                              text_x, pos.text_y, available_width, is_selected);
    }

    render_scroll_indicators(screen, scroll, layout.items_per_page, total);
    GFX_blitButtonGroup((char*[]){"START", "CONTROLS", NULL}, 0, screen, 0);
    GFX_blitButtonGroup((char*[]){"B", "BACK", "A", "SELECT", "Y", "SEARCH", NULL}, 1, screen, 1);
}

void render_search_detail(SDL_Surface* screen, int show_setting,
                          const char* title,
                          const char* m3u_path,
                          PlaylistTrack* tracks, int count,
                          int selected, int scroll) {
    GFX_clear(screen);

    if (Settings_getPlaylistBgArtwork() && m3u_path && m3u_path[0]) {
        SDL_Surface* bg_art = PlaylistArt_get(m3u_path);
        if (bg_art && bg_art->w > 0 && bg_art->h > 0) {
            render_album_art_background(screen, bg_art);
        }
    }

    char truncated[256];
    char header[300];
    snprintf(header, sizeof(header), "%s", title ? title : "Tracks");
    render_screen_header(screen, header, show_setting);

    if (count == 0) {
        render_empty_state(screen, "No tracks", NULL, NULL);
        GFX_blitButtonGroup((char*[]){"B", "BACK", "Y", "SEARCH", NULL}, 1, screen, 1);
        return;
    }

    ListLayout layout = calc_list_layout(screen);
    bool tooltip = Settings_getTooltipArtwork();
    int icon_size = Icons_isLoaded() ? SCALE1(24) : 0;
    int icon_spacing = Icons_isLoaded() ? SCALE1(6) : 0;
    int icon_offset = icon_size + icon_spacing;

    for (int i = 0; i < layout.items_per_page && (scroll + i) < count; i++) {
        int idx = scroll + i;
        bool is_selected = (idx == selected);
        int y = layout.list_y + i * layout.item_h;

        PlaylistTrack* tr = &tracks[idx];
        bool use_icon_slot = Icons_isLoaded() && tooltip;
        int row_icon_offset = use_icon_slot ? icon_offset : 0;

        ListItemPos pos = render_list_item_pill(screen, &layout, tr->name, truncated, y, is_selected, row_icon_offset);

        if (use_icon_slot) {
            int icon_y = y + (layout.item_h - icon_size) / 2;
            SDL_Surface* thumb = TrackArt_getThumbnail(tr->path, icon_size);
            render_list_icon(screen, pos.text_x, icon_y, icon_size, thumb, tr->format, is_selected);
        }

        int text_x = pos.text_x + (use_icon_slot ? icon_offset : 0);
        int available_width = pos.pill_width - SCALE1(BUTTON_PADDING * 2);
        if (use_icon_slot) available_width -= icon_offset;

        render_list_item_text(screen, &search_detail_scroll, tr->name, Fonts_getMedium(),
                              text_x, pos.text_y, available_width, is_selected);
    }

    render_scroll_indicators(screen, scroll, layout.items_per_page, count);
    GFX_blitButtonGroup((char*[]){"START", "CONTROLS", NULL}, 0, screen, 0);
    GFX_blitButtonGroup((char*[]){"B", "BACK", "A", "PLAY", "Y", "SEARCH", NULL}, 1, screen, 1);
}
