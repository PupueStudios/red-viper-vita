#ifndef RV_VITA_COLOR_H
#define RV_VITA_COLOR_H

#define COLOR_TINT_COUNT 6

enum {
    COLOR_MODE_TINT,
    COLOR_MODE_PACK,
    COLOR_MODE_AUTO,
    COLOR_MODE_COUNT
};

extern const char *const color_tint_names[COLOR_TINT_COUNT];
extern const char *const color_mode_names[COLOR_MODE_COUNT];

void color_frame_palette(float out[4][3]);
void color_preview_palette(float out[4][3]);

#endif
