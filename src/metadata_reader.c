#include "metadata_reader.h"
#include "m4a_metadata.h"
#include "player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "audio/dr_flac.h"
#define STB_VORBIS_HEADER_ONLY
#include "audio/stb_vorbis.h"

static void trim_inplace(char* s) {
    if (!s) return;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\0')) s[--len] = '\0';
    char* start = s;
    while (*start == ' ') start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
}

static void copy_field(char* dest, int dest_size, const char* src) {
    if (!dest || dest_size <= 0) return;
    if (!src) {
        dest[0] = '\0';
        return;
    }
    strncpy(dest, src, dest_size - 1);
    dest[dest_size - 1] = '\0';
    trim_inplace(dest);
}

void Metadata_copyNormalized(char* dest, int dest_size, const char* src) {
    if (!dest || dest_size <= 0) return;
    dest[0] = '\0';
    if (!src) return;

    int j = 0;
    bool last_space = true;
    for (int i = 0; src[i] && j < dest_size - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c)) {
            dest[j++] = (char)tolower(c);
            last_space = false;
        } else if (isspace(c) || ispunct(c)) {
            if (!last_space && j < dest_size - 2) {
                dest[j++] = ' ';
                last_space = true;
            }
        }
    }
    while (j > 0 && dest[j - 1] == ' ') j--;
    dest[j] = '\0';
}

typedef struct {
    MetadataTokenFn fn;
    void* userdata;
} TokenCtx;

static void emit_token(const char* token, TokenCtx* ctx) {
    if (!token || !token[0] || strlen(token) < 2) return;
    ctx->fn(token, ctx->userdata);
}

void Metadata_foreachToken(const char* text, MetadataTokenFn fn, void* userdata) {
    if (!text || !fn) return;

    char norm[512];
    Metadata_copyNormalized(norm, sizeof(norm), text);
    if (!norm[0]) return;

    TokenCtx ctx = {fn, userdata};
    char* copy = strdup(norm);
    if (!copy) return;

    char* save = NULL;
    for (char* tok = strtok_r(copy, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
        emit_token(tok, &ctx);
    }
    free(copy);
}

void Metadata_titleFromFilename(const char* filepath, char* title, int title_size) {
    if (!filepath || !title || title_size <= 0) return;
    const char* slash = strrchr(filepath, '/');
    const char* fname = slash ? slash + 1 : filepath;
    copy_field(title, title_size, fname);
    char* dot = strrchr(title, '.');
    if (dot && dot != title) *dot = '\0';
}

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

static void utf16le_to_ascii(char* dest, const uint8_t* src, size_t src_len, size_t max_len) {
    size_t j = 0;
    for (size_t i = 0; i + 1 < src_len && j < max_len - 1; i += 2) {
        uint16_t ch = src[i] | (src[i + 1] << 8);
        if (ch > 0 && ch < 256) dest[j++] = (char)ch;
    }
    dest[j] = '\0';
}

static void read_id3_text(const uint8_t* frame_data, uint32_t frame_size, char* out, int out_size) {
    if (!frame_data || frame_size <= 1 || !out || out_size <= 0) return;
    out[0] = '\0';

    uint8_t encoding = frame_data[0];
    const uint8_t* text_data = &frame_data[1];
    size_t text_len = frame_size - 1;
    char temp[256];
    temp[0] = '\0';

    if (encoding == 0 || encoding == 3) {
        size_t copy_len = text_len < sizeof(temp) - 1 ? text_len : sizeof(temp) - 1;
        memcpy(temp, text_data, copy_len);
        temp[copy_len] = '\0';
    } else if (encoding == 1 && text_len >= 2) {
        bool is_le = (text_data[0] == 0xFF && text_data[1] == 0xFE);
        const uint8_t* p = text_data + 2;
        size_t len = text_len - 2;
        if (is_le) utf16le_to_ascii(temp, p, len, sizeof(temp));
    }

    copy_field(out, out_size, temp);
}

static void parse_mp3_tags(const char* filepath, TrackMetadata* out) {
    FILE* f = fopen(filepath, "rb");
    if (!f) return;

    uint8_t header[10];
    if (fread(header, 1, 10, f) != 10 || header[0] != 'I' || header[1] != 'D' || header[2] != '3') {
        fclose(f);
        return;
    }

    uint8_t version_major = header[3];
    uint32_t tag_size = read_syncsafe_int(&header[6]);
    uint8_t* tag_data = malloc(tag_size);
    if (!tag_data || fread(tag_data, 1, tag_size, f) != tag_size) {
        free(tag_data);
        fclose(f);
        return;
    }
    fclose(f);

    uint32_t pos = 0;
    while (pos + 10 < tag_size) {
        char frame_id[5];
        memcpy(frame_id, &tag_data[pos], 4);
        frame_id[4] = '\0';
        if (frame_id[0] == '\0') break;

        uint32_t frame_size = (version_major == 4)
            ? read_syncsafe_int(&tag_data[pos + 4])
            : read_be32(&tag_data[pos + 4]);
        pos += 10;
        if (frame_size == 0 || pos + frame_size > tag_size) break;

        if (frame_id[0] == 'T') {
            char temp[256];
            read_id3_text(&tag_data[pos], frame_size, temp, sizeof(temp));
            if (strcmp(frame_id, "TIT2") == 0 && temp[0]) copy_field(out->title, sizeof(out->title), temp);
            else if (strcmp(frame_id, "TPE1") == 0 && temp[0]) copy_field(out->artist, sizeof(out->artist), temp);
            else if (strcmp(frame_id, "TPE2") == 0 && temp[0] && !out->artist[0])
                copy_field(out->artist, sizeof(out->artist), temp);
            else if (strcmp(frame_id, "TALB") == 0 && temp[0]) copy_field(out->album, sizeof(out->album), temp);
            else if (strcmp(frame_id, "TCON") == 0 && temp[0]) copy_field(out->genre, sizeof(out->genre), temp);
        }
        pos += frame_size;
    }
    free(tag_data);
}

static void parse_vorbis_field(const char* comment, TrackMetadata* out) {
    const char* eq = strchr(comment, '=');
    if (!eq) return;

    size_t key_len = (size_t)(eq - comment);
    const char* value = eq + 1;

    if (strncasecmp(comment, "TITLE", key_len) == 0 && key_len == 5 && value[0])
        copy_field(out->title, sizeof(out->title), value);
    else if (strncasecmp(comment, "ARTIST", key_len) == 0 && key_len == 6 && value[0])
        copy_field(out->artist, sizeof(out->artist), value);
    else if (strncasecmp(comment, "ALBUM", key_len) == 0 && key_len == 5 && value[0])
        copy_field(out->album, sizeof(out->album), value);
    else if (strncasecmp(comment, "ALBUMARTIST", key_len) == 0 && key_len == 11 && value[0] && !out->artist[0])
        copy_field(out->artist, sizeof(out->artist), value);
    else if (strncasecmp(comment, "GENRE", key_len) == 0 && key_len == 5 && value[0])
        copy_field(out->genre, sizeof(out->genre), value);
}

typedef struct {
    TrackMetadata* meta;
} FlacMetaCtx;

static void flac_meta_cb(void* user, drflac_metadata* md) {
    FlacMetaCtx* ctx = (FlacMetaCtx*)user;
    if (!ctx || !ctx->meta || md->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) return;

    const char* p = md->data.vorbis_comment.pComments;
    for (uint32_t i = 0; i < md->data.vorbis_comment.commentCount && p; i++) {
        uint32_t len = *(const uint32_t*)p;
        p += 4;
        char* comment = malloc(len + 1);
        if (comment) {
            memcpy(comment, p, len);
            comment[len] = '\0';
            parse_vorbis_field(comment, ctx->meta);
            free(comment);
        }
        p += len;
    }
}

static void parse_flac_tags(const char* filepath, TrackMetadata* out) {
    FlacMetaCtx ctx = {out};
    drflac* flac = drflac_open_file_with_metadata(filepath, flac_meta_cb, &ctx, NULL);
    if (flac) drflac_close(flac);
}

static void parse_ogg_tags(const char* filepath, TrackMetadata* out) {
    int error = 0;
    stb_vorbis* v = stb_vorbis_open_filename(filepath, &error, NULL);
    if (!v) return;

    stb_vorbis_comment comments = stb_vorbis_get_comment(v);
    for (int i = 0; i < comments.comment_list_length; i++) {
        if (comments.comment_list[i]) {
            parse_vorbis_field(comments.comment_list[i], out);
        }
    }
    stb_vorbis_close(v);
}

void Metadata_readFromFile(const char* filepath, TrackMetadata* out) {
    if (!filepath || !out) return;
    memset(out, 0, sizeof(*out));
    Metadata_titleFromFilename(filepath, out->title, sizeof(out->title));

    AudioFormat fmt = Player_detectFormat(filepath);
    switch (fmt) {
        case AUDIO_FORMAT_MP3:
            parse_mp3_tags(filepath, out);
            break;
        case AUDIO_FORMAT_FLAC:
            parse_flac_tags(filepath, out);
            break;
        case AUDIO_FORMAT_OGG:
            parse_ogg_tags(filepath, out);
            break;
        case AUDIO_FORMAT_M4A:
        case AUDIO_FORMAT_AAC: {
            M4ATags tags;
            if (M4A_readTags(filepath, &tags) == 0) {
                if (tags.title[0]) copy_field(out->title, sizeof(out->title), tags.title);
                if (tags.artist[0]) copy_field(out->artist, sizeof(out->artist), tags.artist);
                if (tags.album[0]) copy_field(out->album, sizeof(out->album), tags.album);
                if (tags.genre[0]) copy_field(out->genre, sizeof(out->genre), tags.genre);
            }
            break;
        }
        default:
            break;
    }

    if (!out->title[0]) Metadata_titleFromFilename(filepath, out->title, sizeof(out->title));
}
