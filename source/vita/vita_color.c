#include <stdbool.h>

#include "v810_mem.h"
#include "vb_dsp.h"
#include "vb_set.h"
#include "vita.h"
#include "vita_color.h"

static const float tints[COLOR_TINT_COUNT][3] = {
    {1.0f, 0.0f, 0.0f},
    {1.0f, 1.0f, 1.0f},
    {0.2f, 1.0f, 0.3f},
    {1.0f, 0.7f, 0.1f},
    {0.2f, 0.7f, 1.0f},
    {0.8f, 0.3f, 1.0f},
};

const char *const color_tint_names[COLOR_TINT_COUNT] = {"Red", "White", "Green", "Amber", "Blue", "Purple"};
const char *const color_mode_names[COLOR_MODE_COUNT] = {"Tint only", "Colour packs", "Always colour"};

static float shade_level(int brt) {
    int saved_tint = tVBOpt.TINT;
    bool saved_multi = tVBOpt.MULTICOL;
    bool saved_ana = tVBOpt.ANAGLYPH;
    tVBOpt.TINT = 0xFFFFFF;
    tVBOpt.MULTICOL = false;
    tVBOpt.ANAGLYPH = false;
    int packed = video_get_colour(1, brt);
    tVBOpt.TINT = saved_tint;
    tVBOpt.MULTICOL = saved_multi;
    tVBOpt.ANAGLYPH = saved_ana;
    return (packed & 0xFF) / 255.0f;
}

static void palette_for(float out[4][3], const int brt[4]) {
    int t = settings.tint < 0 || settings.tint >= COLOR_TINT_COUNT ? 0 : settings.tint;
    for (int s = 0; s < 4; s++) {
        float level = s == 0 ? 0.0f : shade_level(brt[s]);
        for (int c = 0; c < 3; c++) out[s][c] = tints[t][c] * level;
    }
}

void color_frame_palette(float out[4][3]) {
    int brt[4] = {
        0,
        vb_state->tVIPREG.BRTA,
        vb_state->tVIPREG.BRTB,
        vb_state->tVIPREG.BRTA + vb_state->tVIPREG.BRTB + vb_state->tVIPREG.BRTC,
    };
    palette_for(out, brt);
}

void color_preview_palette(float out[4][3]) {
    int brt[4] = {0, 32, 64, 128};
    palette_for(out, brt);
}
