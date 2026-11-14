#ifndef RV_VITA_UI_H
#define RV_VITA_UI_H

#include <stdbool.h>
#include <stdint.h>
#include <vitaGL.h>

#ifndef RGBA8
#define RGBA8(r, g, b, a) ((((a) & 0xFF) << 24) | (((b) & 0xFF) << 16) | (((g) & 0xFF) << 8) | (((r) & 0xFF) << 0))
#endif

#define COL_TEXT RGBA8(242, 232, 234, 255)
#define COL_TEXT_DIM RGBA8(160, 132, 138, 255)
#define COL_TEXT_FAINT RGBA8(110, 86, 92, 255)
#define COL_ACCENT RGBA8(255, 38, 64, 255)
#define COL_ACCENT_DEEP RGBA8(150, 0, 24, 255)
#define COL_PANEL RGBA8(20, 6, 10, 214)
#define COL_PANEL_SOLID RGBA8(24, 8, 12, 255)
#define COL_BORDER RGBA8(255, 38, 64, 60)
#define COL_SHADOW RGBA8(0, 0, 0, 120)
#define COL_OK RGBA8(80, 220, 140, 255)

enum {
    GLYPH_CROSS,
    GLYPH_CIRCLE,
    GLYPH_TRIANGLE,
    GLYPH_SQUARE,
    GLYPH_START,
    GLYPH_SELECT,
    GLYPH_L,
    GLYPH_R,
    GLYPH_DPAD,
    GLYPH_CONFIRM,
    GLYPH_BACK,
};

typedef struct {
    int glyph;
    const char *label;
} UiHint;

typedef struct {
    GLuint id;
    int w, h;
} UiTexture;

typedef struct {
    float x, y, u, v;
    uint32_t c;
} UiVert;

UiTexture *ui_texture_create(int w, int h, const uint32_t *rgba);
void ui_texture_update(UiTexture *t, const uint32_t *rgba);
UiTexture *ui_texture_load(const char *path);
void ui_texture_free(UiTexture *t);
void ui_texture_filter(UiTexture *t, bool linear);
void ui_draw_texture(const UiTexture *t, float x, float y, float w, float h, uint32_t tint);
void ui_draw_gl_texture(GLuint id, float x, float y, float w, float h, float u0, float v0, float u1, float v1, uint32_t tint);
void ui_flush(void);
void ui_restore_state(void);
void ui_clip_push(float x0, float y0, float x1, float y1);
void ui_clip_pop(void);

void ui_init(void);
void ui_shutdown(void);
void ui_frame_begin(void);
void ui_frame_end(void);
float ui_time(void);
float ui_dt(void);

uint32_t ui_alpha(uint32_t color, float a);
uint32_t ui_mix(uint32_t a, uint32_t b, float t);
float ui_approach(float current, float target, float speed);
float ui_ease_out(float t);

void ui_rect(float x, float y, float w, float h, uint32_t c);
void ui_round_rect(float x, float y, float w, float h, float r, uint32_t c);
void ui_round_outline(float x, float y, float w, float h, float r, uint32_t c);
void ui_gradient_v(float x, float y, float w, float h, uint32_t top, uint32_t bottom);
void ui_gradient_h(float x, float y, float w, float h, uint32_t left, uint32_t right);
void ui_line(float x0, float y0, float x1, float y1, uint32_t c);
void ui_thick_line(float x0, float y0, float x1, float y1, float t, uint32_t c);
void ui_circle(float x, float y, float r, uint32_t c);
void ui_ring(float x, float y, float r, float t, uint32_t c);

int ui_text(float x, float y, uint32_t c, float scale, const char *s);
int ui_text_right(float x, float y, uint32_t c, float scale, const char *s);
int ui_text_center(float cx, float y, uint32_t c, float scale, const char *s);
int ui_text_width(float scale, const char *s);
void ui_text_clipped(float x, float y, float max_w, uint32_t c, float scale, const char *s, float scroll_t);

void ui_dot_text(float x, float y, float dot, uint32_t c, const char *s);
float ui_dot_text_width(float dot, const char *s);

float ui_glyph(int glyph, float x, float cy, float alpha);
void ui_hints(const UiHint *hints, int count);

void ui_background(void);
void ui_header(const char *subtitle);
void ui_toast(const char *msg);
void ui_draw_toast(void);
void ui_icon_folder(float x, float y, uint32_t c);
void ui_icon_file(float x, float y, float scale, uint32_t c);
void ui_icon_drive(float x, float y, uint32_t c);
void ui_spinner(float cx, float cy, float r, uint32_t c);

#endif
