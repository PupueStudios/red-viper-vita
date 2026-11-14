#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v810_mem.h"
#include "vb_dsp.h"
#include "vb_set.h"
#include "vita.h"
#include "vita_color.h"

#define HEX(v) {(((v) >> 16) & 0xFF) / 255.0f, (((v) >> 8) & 0xFF) / 255.0f, ((v) & 0xFF) / 255.0f}
#define COLOR_GAMES_PATH RV_DATA_DIR "/colors.ini"
#define AUTO_SCHEME -2

const char *const color_mode_names[COLOR_MODE_COUNT] = {"Tint", "Multicolor", "Gradient"};

const ColorRgb color_tints[COLOR_TINT_COUNT] = {
    {1.0f, 0.0f, 0.0f},
    {0.9f, 0.3f, 0.1f},
    {1.0f, 0.85f, 0.1f},
    {0.25f, 1.0f, 0.1f},
    {0.0f, 1.0f, 0.45f},
    {0.0f, 1.0f, 0.85f},
    {0.0f, 0.85f, 1.0f},
    {0.15f, 1.0f, 1.0f},
    {0.75f, 0.65f, 1.0f},
    {1.0f, 1.0f, 1.0f},
    {1.0f, 0.3f, 0.2f},
};
const char *const color_tint_names[COLOR_TINT_COUNT] = {"Red", "Orange", "Yellow", "Lime", "Green", "Mint", "Sky", "Cyan", "Lavender", "White", "Coral"};

const ColorRgb color_gradients[COLOR_GRADIENT_COUNT][5] = {
    {{0.02f, 0.03f, 0.02f}, {0.05f, 0.20f, 0.10f}, {0.10f, 0.45f, 0.40f}, {0.55f, 0.75f, 0.35f}, {0.97f, 0.96f, 0.85f}},
    {{0.01f, 0.02f, 0.04f}, {0.05f, 0.15f, 0.35f}, {0.10f, 0.35f, 0.65f}, {0.55f, 0.80f, 0.90f}, {0.95f, 0.98f, 1.00f}},
    {{0.04f, 0.01f, 0.05f}, {0.30f, 0.05f, 0.35f}, {0.75f, 0.20f, 0.25f}, {0.95f, 0.55f, 0.20f}, {1.00f, 0.92f, 0.75f}},
    {{0.03f, 0.01f, 0.00f}, {0.35f, 0.04f, 0.02f}, {0.65f, 0.15f, 0.02f}, {0.95f, 0.55f, 0.10f}, {1.00f, 0.95f, 0.75f}},
    {{0.02f, 0.02f, 0.05f}, {0.15f, 0.15f, 0.40f}, {0.40f, 0.35f, 0.70f}, {0.75f, 0.75f, 0.95f}, {0.98f, 0.98f, 1.00f}},
    {{0.02f, 0.03f, 0.00f}, {0.15f, 0.30f, 0.02f}, {0.35f, 0.55f, 0.05f}, {0.65f, 0.85f, 0.15f}, {0.95f, 1.00f, 0.70f}},
};
const char *const color_gradient_names[COLOR_GRADIENT_COUNT] = {"Jade", "Ocean", "Sunset", "Ember", "Frost", "Toxic"};

const ColorRgb color_multicolors[COLOR_MULTICOLOR_COUNT][4] = {
    {HEX(0x050A24), HEX(0xE0301E), HEX(0x2E7DF0), HEX(0xFFE14D)},
    {HEX(0x0B0614), HEX(0x7A1FA2), HEX(0x00B8D4), HEX(0xF4FF81)},
    {HEX(0x160A1C), HEX(0x6A3FB5), HEX(0xFF5FA2), HEX(0xB8F4FF)},
    {HEX(0x080200), HEX(0x8C2A0A), HEX(0x1DBB00), HEX(0xFFD800)},
    {HEX(0x03141A), HEX(0x8E2C6B), HEX(0x1FB3A6), HEX(0xFFD9A0)},
    {HEX(0x000000), HEX(0xAA00AA), HEX(0x00AAAA), HEX(0xFFFFFF)},
    {{0.02f, 0.03f, 0.02f}, {0.10f, 0.45f, 0.40f}, {0.55f, 0.75f, 0.35f}, {0.97f, 0.96f, 0.85f}},
    {{0.01f, 0.02f, 0.04f}, {0.10f, 0.35f, 0.65f}, {0.55f, 0.80f, 0.90f}, {0.95f, 0.98f, 1.00f}},
    {{0.04f, 0.01f, 0.05f}, {0.75f, 0.20f, 0.25f}, {0.95f, 0.55f, 0.20f}, {1.00f, 0.92f, 0.75f}},
    {{0.03f, 0.01f, 0.00f}, {0.65f, 0.15f, 0.02f}, {0.95f, 0.55f, 0.10f}, {1.00f, 0.95f, 0.75f}},
    {{0.02f, 0.02f, 0.05f}, {0.40f, 0.35f, 0.70f}, {0.75f, 0.75f, 0.95f}, {0.98f, 0.98f, 1.00f}},
    {{0.02f, 0.03f, 0.00f}, {0.35f, 0.55f, 0.05f}, {0.65f, 0.85f, 0.15f}, {0.95f, 1.00f, 0.70f}},
    {HEX(0x9BBC0F), HEX(0x6A8F1C), HEX(0x306230), HEX(0x0F380F)},
    {HEX(0x0F380F), HEX(0x306230), HEX(0x8BAC0F), HEX(0x9BBC0F)},
    {HEX(0xF8E8C8), HEX(0xD89048), HEX(0xA82820), HEX(0x301850)},
    {HEX(0xF4EFE1), HEX(0x8C877B), HEX(0x4A463F), HEX(0x141414)},
};
const char *const color_multicolor_names[COLOR_MULTICOLOR_COUNT] = {"Arcade", "Neon", "Candy", "Fire & leaf", "Tropical", "Retro PC", "Jade", "Ocean", "Sunset", "Ember", "Frost", "Toxic", "LCD green", "LCD green, dark", "Super warm", "Paper"};

enum {
    MC_ARCADE = 0,
    MC_NEON = 1,
    MC_CANDY = 2,
    MC_FIRE_LEAF = 3,
    MC_OCEAN = 7,
    MC_SUNSET = 8,
};

typedef struct {
    uint32_t crc;
    const char *name_contains;
    int multicolor;
} Suggestion;

static const Suggestion suggestions[] = {
    {0x133e9372, "wario land", AUTO_SCHEME},
    {0xbb71b522, "3-d tetris", AUTO_SCHEME},
    {0xe81a3703, "bound high", AUTO_SCHEME},
    {0x193c1a2f, "dragon hopper", AUTO_SCHEME},
    {0x2199af41, "golf", MC_FIRE_LEAF},
    {0x6ba07915, "virtual golf", MC_FIRE_LEAF},
    {0x83cb6a00, "innsmouth", AUTO_SCHEME},
    {0xdf4d56b4, "funky bowling", MC_CANDY},
    {0xf3cd40dd, "niko-chan", MC_ARCADE},
    {0x19bb2dfb, "panic bomber", AUTO_SCHEME},
    {0x40498f5e, "panibon", AUTO_SCHEME},
    {0xaa10a7b4, "red alarm", AUTO_SCHEME},
    {0x7e85c45d, "red alarm", AUTO_SCHEME},
    {0x44788197, "gundam", MC_OCEAN},
    {0xfa44402d, "space invaders", AUTO_SCHEME},
    {0x60895693, "space squash", MC_NEON},
    {0x3ccb67ae, "v-tetris", AUTO_SCHEME},
    {0x4c32ba5e, "vertical force", AUTO_SCHEME},
    {0x9e9b8b92, "vertical force", AUTO_SCHEME},
    {0x20688279, "virtual bowling", MC_SUNSET},
    {0x526cc969, "virtual fishing", MC_OCEAN},
    {0x8989fe0a, "virtual lab", MC_CANDY},
    {0x736b40d6, "league baseball", MC_ARCADE},
    {0x9ba8bb5e, "yakyuu", MC_ARCADE},
    {0x82a95e51, "waterworld", MC_OCEAN},
};

static bool game_active = false;
static uint32_t game_crc = 0;
static int global_choice[4];
static bool has_override = false;
static float gradient_reference = 0;

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static float clampf(float v) {
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

static float shade_level(int brt) {
    int packed = video_get_colour(1, brt);
    return (packed & 0xFF) / 255.0f;
}

static void white_tint_begin(int *saved_tint, bool *saved_multi, bool *saved_ana) {
    *saved_tint = tVBOpt.TINT;
    *saved_multi = tVBOpt.MULTICOL;
    *saved_ana = tVBOpt.ANAGLYPH;
    tVBOpt.TINT = 0xFFFFFF;
    tVBOpt.MULTICOL = false;
    tVBOpt.ANAGLYPH = false;
}

static void white_tint_end(int saved_tint, bool saved_multi, bool saved_ana) {
    tVBOpt.TINT = saved_tint;
    tVBOpt.MULTICOL = saved_multi;
    tVBOpt.ANAGLYPH = saved_ana;
}

static ColorRgb gradient_sample(const ColorRgb *stops, float luma) {
    float scaled = clampf(luma) * 4.0f;
    int seg = (int)floorf(scaled);
    if (seg > 3) seg = 3;
    if (seg < 0) seg = 0;
    float f = scaled - seg;
    ColorRgb a = stops[seg], b = stops[seg + 1];
    ColorRgb out = {a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f};
    return out;
}

static void set_rgb(float out[3], ColorRgb c) {
    out[0] = c.r;
    out[1] = c.g;
    out[2] = c.b;
}

static void mix_into(float out[3], ColorRgb bg, ColorRgb fg, float t) {
    out[0] = bg.r + (fg.r - bg.r) * t;
    out[1] = bg.g + (fg.g - bg.g) * t;
    out[2] = bg.b + (fg.b - bg.b) * t;
}

static void palette_for(float out[4][3], int brt[4], bool live) {
    int saved_tint;
    bool saved_multi, saved_ana;
    white_tint_begin(&saved_tint, &saved_multi, &saved_ana);
    float level[4];
    for (int s = 0; s < 4; s++) level[s] = s == 0 ? 0.0f : shade_level(brt[s]);
    white_tint_end(saved_tint, saved_multi, saved_ana);

    switch (settings.color_mode) {
        case COLOR_MULTICOLOR: {
            const ColorRgb *p = color_multicolors[clampi(settings.multicolor, 0, COLOR_MULTICOLOR_COUNT - 1)];
            float fade = clampf(level[3]);
            set_rgb(out[0], p[0]);
            for (int s = 1; s < 4; s++) {
                if (brt[s] <= 0) set_rgb(out[s], p[0]);
                else mix_into(out[s], p[0], p[s], fade);
            }
            break;
        }
        case COLOR_GRADIENT: {
            const ColorRgb *stops = color_gradients[clampi(settings.gradient, 0, COLOR_GRADIENT_COUNT - 1)];
            float ref = level[3];
            if (live) {
                if (ref > gradient_reference) gradient_reference = ref;
                ref = gradient_reference;
            }
            if (ref <= 0.001f) ref = 1.0f;
            float fade = clampf(level[3] / ref);
            set_rgb(out[0], stops[0]);
            for (int s = 1; s < 4; s++) {
                if (brt[s] <= 0) {
                    set_rgb(out[s], stops[0]);
                    continue;
                }
                float luma = level[3] > 0.001f ? clampf(level[s] / level[3]) : (float)s / 3.0f;
                mix_into(out[s], stops[0], gradient_sample(stops, luma), fade);
            }
            break;
        }
        default: {
            ColorRgb t = color_tints[clampi(settings.tint, 0, COLOR_TINT_COUNT - 1)];
            out[0][0] = out[0][1] = out[0][2] = 0;
            for (int s = 1; s < 4; s++) {
                out[s][0] = t.r * level[s];
                out[s][1] = t.g * level[s];
                out[s][2] = t.b * level[s];
            }
            break;
        }
    }
}

void color_frame_palette(float out[4][3]) {
    int brt[4] = {
        0,
        vb_state->tVIPREG.BRTA,
        vb_state->tVIPREG.BRTB,
        vb_state->tVIPREG.BRTA + vb_state->tVIPREG.BRTB + vb_state->tVIPREG.BRTC,
    };
    palette_for(out, brt, true);
}

void color_preview_palette(float out[4][3]) {
    int brt[4] = {0, 32, 64, 128};
    palette_for(out, brt, false);
}

static void lower_copy(char *dst, size_t len, const char *src) {
    size_t i = 0;
    for (; src[i] && i + 1 < len; i++) dst[i] = (char)tolower((unsigned char)src[i]);
    dst[i] = 0;
}

static bool load_override(uint32_t crc, int choice[4]) {
    FILE *f = fopen(COLOR_GAMES_PATH, "r");
    if (!f) return false;
    char line[128];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        unsigned int c;
        int m, t, mc, g;
        if (sscanf(line, "%x=%d,%d,%d,%d", &c, &m, &t, &mc, &g) == 5 && c == crc) {
            choice[0] = clampi(m, 0, COLOR_MODE_COUNT - 1);
            choice[1] = clampi(t, 0, COLOR_TINT_COUNT - 1);
            choice[2] = clampi(mc, 0, COLOR_MULTICOLOR_COUNT - 1);
            choice[3] = clampi(g, 0, COLOR_GRADIENT_COUNT - 1);
            found = true;
        }
    }
    fclose(f);
    return found;
}

static void save_override(uint32_t crc, const int choice[4]) {
    char *kept = NULL;
    size_t kept_len = 0;
    FILE *f = fopen(COLOR_GAMES_PATH, "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            unsigned int c;
            if (sscanf(line, "%x=", &c) == 1 && c == crc) continue;
            size_t n = strlen(line);
            char *grown = realloc(kept, kept_len + n + 1);
            if (!grown) break;
            kept = grown;
            memcpy(kept + kept_len, line, n + 1);
            kept_len += n;
        }
        fclose(f);
    }
    f = fopen(COLOR_GAMES_PATH, "w");
    if (f) {
        if (kept) fwrite(kept, 1, kept_len, f);
        fprintf(f, "%08x=%d,%d,%d,%d\n", (unsigned)crc, choice[0], choice[1], choice[2], choice[3]);
        fclose(f);
    }
    free(kept);
}

static void read_choice(int choice[4]) {
    choice[0] = settings.color_mode;
    choice[1] = settings.tint;
    choice[2] = settings.multicolor;
    choice[3] = settings.gradient;
}

static void apply_choice(const int choice[4]) {
    settings.color_mode = choice[0];
    settings.tint = choice[1];
    settings.multicolor = choice[2];
    settings.gradient = choice[3];
}

void color_on_game_loaded(uint32_t crc, const char *rom_name) {
    if (!game_active) read_choice(global_choice);
    game_active = true;
    game_crc = crc;
    gradient_reference = 0;
    int choice[4];
    memcpy(choice, global_choice, sizeof(choice));
    has_override = load_override(crc, choice);
    if (!has_override) {
        char lower[256];
        lower_copy(lower, sizeof(lower), rom_name ? rom_name : "");
        const Suggestion *hit = NULL;
        for (unsigned i = 0; i < sizeof(suggestions) / sizeof(suggestions[0]) && !hit; i++)
            if (suggestions[i].crc == crc) hit = &suggestions[i];
        for (unsigned i = 0; i < sizeof(suggestions) / sizeof(suggestions[0]) && !hit; i++)
            if (strstr(lower, suggestions[i].name_contains)) hit = &suggestions[i];
        if (hit && hit->multicolor >= 0) {
            choice[0] = COLOR_MULTICOLOR;
            choice[2] = hit->multicolor;
        }
    }
    apply_choice(choice);
}

void color_on_game_closed(void) {
    if (!game_active) return;
    apply_choice(global_choice);
    game_active = false;
    has_override = false;
}

void color_on_changed(void) {
    gradient_reference = 0;
    if (game_active) {
        int choice[4];
        read_choice(choice);
        save_override(game_crc, choice);
        has_override = true;
    }
}

void color_global_choice(int out[4]) {
    if (game_active) memcpy(out, global_choice, sizeof(global_choice));
    else read_choice(out);
}

bool color_game_has_override(void) {
    return has_override;
}
