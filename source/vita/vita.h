#ifndef RV_VITA_H
#define RV_VITA_H

#include <stdbool.h>
#include <stdint.h>

#define RV_DATA_DIR "ux0:data/red-viper"
#define RV_ROM_DIR RV_DATA_DIR "/roms"
#define RV_CONFIG_PATH RV_DATA_DIR "/vita.ini"

#define SCREEN_W 960
#define SCREEN_H 544
#define VB_W 384
#define VB_H 224

#define STATE_SLOTS 10

enum {
    SCALE_INTEGER,
    SCALE_FIT,
    SCALE_STRETCH,
    SCALE_COUNT
};

enum {
    STEREO_LEFT,
    STEREO_RIGHT,
    STEREO_ANAGLYPH,
    STEREO_SIDE_BY_SIDE,
    STEREO_COUNT
};

enum {
    ANAGLYPH_RED_CYAN,
    ANAGLYPH_RED_BLUE,
    ANAGLYPH_RED_GREEN,
    ANAGLYPH_COUNT
};

enum {
    CONTROLS_STANDARD,
    CONTROLS_DUAL_PAD,
    CONTROLS_COUNT
};

typedef struct {
    int scale_mode;
    int smooth;
    int stereo;
    int anaglyph;
    int tint;
    int color_mode;
    int show_fps;
    int sound;
    int volume;
    int controls;
    int swap_ab;
    int ff_toggle;
    int overclock;
    int led_grid;
    char last_dir[512];
} VitaSettings;

typedef struct {
    const char *name;
    uint32_t tint;
    int multicol_id;
} VitaPalette;

extern VitaSettings settings;

typedef struct {
    uint32_t held;
    uint32_t pressed;
    uint32_t repeat;
    int touch_front;
    int touch_front_tap;
    int touch_x, touch_y;
    int lx, ly, rx, ry;
} InputFrame;

enum {
    BTN_UP = 1 << 0,
    BTN_DOWN = 1 << 1,
    BTN_LEFT = 1 << 2,
    BTN_RIGHT = 1 << 3,
    BTN_CONFIRM = 1 << 4,
    BTN_BACK = 1 << 5,
    BTN_TRIANGLE = 1 << 6,
    BTN_SQUARE = 1 << 7,
    BTN_L = 1 << 8,
    BTN_R = 1 << 9,
    BTN_START = 1 << 10,
    BTN_SELECT = 1 << 11,
};

void settings_defaults(void);
void settings_load(void);
void settings_save(void);
void settings_apply(void);

bool jit_probe(void);

bool audio_init(void);
void audio_shutdown(void);
void audio_clear(void);
int audio_fill_percent(void);

void input_init(void);
void input_poll(InputFrame *in);
uint16_t input_vb_keys(const InputFrame *in);
bool input_menu_combo(const InputFrame *in);
void input_capture_ps_button(bool capture);
void input_release_ps_button_now(void);
bool input_game_blocked(void);
bool input_fast_forward(const InputFrame *in);
bool input_confirm_is_circle(void);
void input_set_fast_forward_latch(bool on);
bool input_get_fast_forward_latch(void);

void video_init_vita(void);
void video_shutdown_vita(void);
void video_present_game(float dim);
void video_draw_game_at(float x, float y, float w, float h, uint32_t tint);
void video_game_rect(float *x, float *y, float *w, float *h);
bool video_make_thumbnail(uint32_t *dst, int w, int h);
void video_apply_filter(void);
void video_clear(void);
void video_recompose(void);
void video_color_refresh(bool game_loaded);
bool vita_shaders_failed(void);

#endif
