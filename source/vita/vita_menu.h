#ifndef RV_VITA_MENU_H
#define RV_VITA_MENU_H

#include <stdbool.h>
#include "vita.h"

typedef enum {
    ACT_NONE,
    ACT_LAUNCH,
    ACT_RESUME,
    ACT_RESET,
    ACT_CLOSE,
    ACT_SAVE_STATE,
    ACT_LOAD_STATE,
    ACT_QUIT,
} MenuAction;

typedef enum {
    SCREEN_BROWSER,
    SCREEN_PAUSE,
    SCREEN_SETTINGS,
} MenuScreen;

void menu_init(void);
void menu_open_browser(void);
void menu_open_pause(const char *game_name);
MenuScreen menu_screen(void);
MenuAction menu_update(const InputFrame *in, bool game_loaded);
void menu_draw(bool game_loaded);
const char *menu_selected_path(void);
int menu_state_slot(void);
void menu_state_changed(int slot);
void menu_show_message(const char *title, const char *body);
void menu_draw_loading(const char *name, int percent);
void menu_draw_fps(float fps, float speed);

bool slot_thumbnail_path(int slot, char *out, int len);

#endif
