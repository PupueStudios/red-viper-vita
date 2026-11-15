#ifndef RV_VITA_COLOR_H
#define RV_VITA_COLOR_H

#define COLOR_TINT_COUNT 6

extern const char *const color_tint_names[COLOR_TINT_COUNT];

void color_frame_palette(float out[4][3]);
void color_preview_palette(float out[4][3]);

#endif
