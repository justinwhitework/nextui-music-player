#include <stdio.h>
#include <string.h>

#include "defines.h"
#include "api.h"
#include "ui_playlist.h"
#include "ui_fonts.h"
#include "ui_icons.h"
#include "ui_utils.h"
#include "ui_album_art.h"
#include "playlist_art.h"
#include "track_art.h"
#include "settings.h"
#include "module_common.h"

static ScrollTextState playlist_scroll = {0};

void render_playlist_list(SDL_Surface* screen, int show_setting,
                          const char* dir_title,
                          PlaylistInfo* playlists, int count,
                          int selected, int scroll) {
    GFX_clear(screen);

    char truncated[256];
    const char* header = (dir_title && dir_title[0]) ? dir_title : "Playlists";
    render_screen_header(screen, header, show_setting);

    if (count == 0) {
        render_empty_state(screen, "No playlists here", "Press Y to create a playlist", "NEW");
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

        PlaylistInfo* pl = &playlists[idx];
        char display[256];
        if (pl->is_folder) {
            if (Icons_isLoaded()) {
                snprintf(display, sizeof(display), "%s", pl->name);
            } else {
                snprintf(display, sizeof(display), "[%s]", pl->name);
            }
        } else {
            snprintf(display, sizeof(display), "%s (%d)", pl->name, pl->track_count);
        }

        bool use_icon_slot = Icons_isLoaded() && (pl->is_folder || tooltip);
        int row_icon_offset = use_icon_slot ? icon_offset : 0;

        ListItemPos pos = render_list_item_pill(screen, &layout, display, truncated, y, is_selected, row_icon_offset);

        if (use_icon_slot) {
            int icon_y = y + (layout.item_h - icon_size) / 2;
            int icon_x = pos.text_x;
            if (pl->is_folder) {
                SDL_Surface* icon = Icons_getFolder(is_selected);
                if (icon) {
                    SDL_Rect src_rect = {0, 0, icon->w, icon->h};
                    SDL_Rect dst_rect = {icon_x, icon_y, icon_size, icon_size};
                    SDL_BlitScaled(icon, &src_rect, screen, &dst_rect);
                }
            } else if (tooltip) {
                SDL_Surface* thumb = PlaylistArt_getThumbnail(pl->path, icon_size);
                if (thumb) {
                    SDL_Rect dst = {icon_x, icon_y, icon_size, icon_size};
                    SDL_BlitScaled(thumb, NULL, screen, &dst);
                }
            }
        }

        int text_x = pos.text_x + (use_icon_slot ? icon_offset : 0);
        int available_width = pos.pill_width - SCALE1(BUTTON_PADDING * 2);
        if (use_icon_slot) {
            available_width -= icon_offset;
        }
        render_list_item_text(screen, &playlist_scroll, display, Fonts_getMedium(),
                              text_x, pos.text_y, available_width, is_selected);
    }

    render_scroll_indicators(screen, scroll, layout.items_per_page, count);

    GFX_blitButtonGroup((char*[]){"START", "CONTROLS", NULL}, 0, screen, 0);
    GFX_blitButtonGroup((char*[]){"B", "BACK", "A", "SELECT", NULL}, 1, screen, 1);
}

void render_playlist_detail(SDL_Surface* screen, int show_setting,
                            const char* playlist_name,
                            const char* playlist_m3u_path,
                            PlaylistTrack* tracks, int count,
                            int selected, int scroll) {
    GFX_clear(screen);

    if (Settings_getPlaylistBgArtwork() && playlist_m3u_path && playlist_m3u_path[0]) {
        SDL_Surface* bg_art = PlaylistArt_get(playlist_m3u_path);
        if (bg_art && bg_art->w > 0 && bg_art->h > 0) {
            render_album_art_background(screen, bg_art);
        }
    }

    char truncated[256];

    char title[300];
    snprintf(title, sizeof(title), "Playlist %s", playlist_name);
    render_screen_header(screen, title, show_setting);

    if (count == 0) {
        render_empty_state(screen, "No tracks in playlist", "Add tracks from the music browser", NULL);
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

        char display[256];
        PlaylistTrack* track = &tracks[idx];
        snprintf(display, sizeof(display), "%s", track->name);

        ListItemPos pos = render_list_item_pill(screen, &layout, display, truncated, y, is_selected, icon_offset);

        if (Icons_isLoaded()) {
            int icon_y = y + (layout.item_h - icon_size) / 2;
            int icon_x = pos.text_x;
            SDL_Surface* artwork = NULL;
            if (tooltip) {
                TrackArt_request(track->path);
                artwork = TrackArt_getThumbnail(track->path, icon_size);
            }
            render_list_icon(screen, icon_x, icon_y, icon_size, artwork, tracks[idx].format, is_selected);
        }

        int text_x = pos.text_x + icon_offset;
        int available_width = pos.pill_width - SCALE1(BUTTON_PADDING * 2) - icon_offset;
        render_list_item_text(screen, &playlist_scroll, display, Fonts_getMedium(),
                              text_x, pos.text_y, available_width, is_selected);
    }

    render_scroll_indicators(screen, scroll, layout.items_per_page, count);
}

bool playlist_list_needs_scroll_refresh(void) {
    return ScrollText_isScrolling(&playlist_scroll);
}

bool playlist_list_scroll_needs_render(void) {
    return ScrollText_needsRender(&playlist_scroll);
}

void playlist_list_animate_scroll(void) {
    ScrollText_animateOnly(&playlist_scroll);
}
