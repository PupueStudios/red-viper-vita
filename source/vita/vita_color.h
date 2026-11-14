#ifndef RV_VITA_COLOR_H
#define RV_VITA_COLOR_H

#include <stdbool.h>
#include <stdint.h>

enum {
    COLOR_TINT,
    COLOR_MULTICOLOR,
    COLOR_GRADIENT,
    COLOR_MODE_COUNT
};

typedef struct {
    float r, g, b;
} ColorRgb;

#define COLOR_TINT_COUNT 11
#define COLOR_GRADIENT_COUNT 6
#define COLOR_MULTICOLOR_COUNT 16

extern const ColorRgb color_tints[COLOR_TINT_COUNT];
extern const char *const color_tint_names[COLOR_TINT_COUNT];
extern const ColorRgb color_gradients[COLOR_GRADIENT_COUNT][5];
extern const char *const color_gradient_names[COLOR_GRADIENT_COUNT];
extern const ColorRgb color_multicolors[COLOR_MULTICOLOR_COUNT][4];
extern const char *const color_multicolor_names[COLOR_MULTICOLOR_COUNT];
extern const char *const color_mode_names[COLOR_MODE_COUNT];

void color_frame_palette(float out[4][3]);
void color_preview_palette(float out[4][3]);
void color_on_game_loaded(uint32_t crc, const char *rom_name);
void color_on_game_closed(void);
void color_on_changed(void);
void color_global_choice(int out[4]);
bool color_game_has_override(void);

#endif
