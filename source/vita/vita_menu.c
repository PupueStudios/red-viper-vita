#include <ctype.h>
#include <math.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/rtc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "vb_types.h"
#include "vb_gui.h"
#include "vb_set.h"
#include "video_hard.h"
#include "vita.h"
#include "vita_color.h"
#include "vita_menu.h"
#include "vita_ui.h"

#define THUMB_W 192
#define THUMB_H 112
#define LIST_X 32
#define LIST_Y 92
#define LIST_W 560
#define ROW_H 42
#define LIST_ROWS 9

char *get_savestate_path(int state, bool write);

typedef enum {
    ENTRY_DRIVE,
    ENTRY_DIR,
    ENTRY_ROM,
} EntryType;

typedef struct {
    char name[256];
    EntryType type;
    int64_t size;
} Entry;

typedef enum {
    ROW_HEADER,
    ROW_CHOICE,
    ROW_SLIDER,
    ROW_ACTION,
    ROW_PALETTE,
} RowKind;

typedef struct {
    RowKind kind;
    const char *label;
    const char *desc;
    int *value;
    int count;
    const char *const *names;
} SettingRow;

typedef struct {
    bool active;
    bool is_confirm;
    char title[64];
    char body[256];
    int choice;
    MenuAction on_yes;
    float born;
} Dialog;

static MenuScreen screen = SCREEN_BROWSER;
static MenuScreen settings_return = SCREEN_BROWSER;
static float screen_born = 0;

static char cur_path[512];
static Entry *entries = NULL;
static int entry_count = 0;
static int entry_cap = 0;
static int browser_sel = 0;
static float browser_scroll = 0;
static float browser_hl = 0;
static float browser_sel_time = 0;
static char selected_path[1024];

static int pause_sel = 0;
static float pause_hl = 0;
static int state_slot = 0;
static char pause_game[256];
static UiTexture *thumb_tex = NULL;
static uint32_t thumb_pixels[192 * 112];
static int thumb_loaded_slot = -2;
static bool thumb_valid = false;
static char thumb_time[48];
static float slot_anim = 0;

static int settings_sel = 1;
static float settings_scroll = 0;
static float settings_hl = 0;
static int reset_dummy = 0;

static Dialog dialog;

#define ART_DIR "app0:assets/images"
#define USER_ART_DIR RV_DATA_DIR "/images"

typedef struct {
    char key[96];
    char base[96];
} ArtIndex;

static ArtIndex *art_index = NULL;
static int art_index_count = 0;
static UiTexture *art_tex = NULL;
static char art_name[256];
static float art_born = 0;

static void load_art_index(void);

static const char *const scale_names[] = {"Integer 2x", "Fit to screen", "Stretch"};
static const char *const filter_names[] = {"Sharp", "Smooth"};
static const char *const onoff_names[] = {"Off", "On"};
static const char *const stereo_names[] = {"Left eye", "Right eye", "Anaglyph 3D", "Side by side"};
static const char *const anaglyph_names[] = {"Red / Cyan", "Red / Blue", "Red / Green"};
static const char *const controls_names[] = {"Standard", "Dual D-Pad"};
static const char *const ff_names[] = {"Hold Triangle", "Toggle Triangle"};
static const char *const clock_names[] = {"333 MHz", "444 MHz"};

static SettingRow rows[] = {
    {ROW_HEADER, "Display", NULL, NULL, 0, NULL},
    {ROW_CHOICE, "Scaling", "Integer scaling keeps every LED perfectly square. Fit fills the height.", &settings.scale_mode, SCALE_COUNT, scale_names},
    {ROW_CHOICE, "Filtering", "Smooth softens pixels when the picture is not integer scaled.", &settings.smooth, 2, filter_names},
    {ROW_CHOICE, "LED grid", "Draws a subtle mesh between pixels, like the original mirror display.", &settings.led_grid, 2, onoff_names},
    {ROW_PALETTE, "Colour", "The colour the Virtual Boy's LEDs are shown in.", NULL, 0, NULL},
    {ROW_CHOICE, "Stereo view", "The Vita has one screen: pick an eye, wear anaglyph glasses, or view side by side.", &settings.stereo, STEREO_COUNT, stereo_names},
    {ROW_CHOICE, "Anaglyph glasses", "Lens colours used when Stereo view is set to Anaglyph 3D.", &settings.anaglyph, ANAGLYPH_COUNT, anaglyph_names},
    {ROW_CHOICE, "Show FPS", "Shows emulation speed in the corner while playing.", &settings.show_fps, 2, onoff_names},
    {ROW_HEADER, "Audio", NULL, NULL, 0, NULL},
    {ROW_CHOICE, "Sound", "Enables the emulated VSU sound chip.", &settings.sound, 2, onoff_names},
    {ROW_SLIDER, "Volume", "Output volume.", &settings.volume, 11, NULL},
    {ROW_HEADER, "Controls", NULL, NULL, 0, NULL},
    {ROW_CHOICE, "Layout", "Standard: right stick is the right D-Pad. Dual D-Pad: face buttons are the right D-Pad, A/B on the rear touchpad.", &settings.controls, CONTROLS_COUNT, controls_names},
    {ROW_CHOICE, "Swap A / B", "Swaps the A and B buttons.", &settings.swap_ab, 2, onoff_names},
    {ROW_CHOICE, "Fast-forward", "How the Triangle button controls fast-forward in the Standard layout.", &settings.ff_toggle, 2, ff_names},
    {ROW_HEADER, "System", NULL, NULL, 0, NULL},
    {ROW_CHOICE, "CPU clock", "444 MHz gives the dynarec more headroom at the cost of battery life.", &settings.overclock, 2, clock_names},
    {ROW_ACTION, "Restore defaults", "Resets every option on this page.", &reset_dummy, 0, NULL},
};
#define ROW_COUNT ((int)(sizeof(rows) / sizeof(rows[0])))

typedef struct {
    const char *label;
    int id;
} PauseItem;

enum {
    PI_RESUME,
    PI_SAVE,
    PI_LOAD,
    PI_FF,
    PI_RESET,
    PI_SETTINGS,
    PI_CLOSE,
    PI_COUNT
};

static const PauseItem pause_items[PI_COUNT] = {
    {"Resume", PI_RESUME},
    {"Save State", PI_SAVE},
    {"Load State", PI_LOAD},
    {"Fast-forward", PI_FF},
    {"Reset", PI_RESET},
    {"Settings", PI_SETTINGS},
    {"Close Game", PI_CLOSE},
};

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static bool has_ext(const char *name, const char *ext) {
    const char *dot = strrchr(name, '.');
    return dot && strcasecmp(dot, ext) == 0;
}

static int entry_cmp(const void *a, const void *b) {
    const Entry *ea = a, *eb = b;
    if (ea->type != eb->type) return ea->type - eb->type;
    return strcasecmp(ea->name, eb->name);
}

static void push_entry(const char *name, EntryType type, int64_t size) {
    if (entry_count >= entry_cap) {
        int ncap = entry_cap ? entry_cap * 2 : 64;
        Entry *n = realloc(entries, ncap * sizeof(Entry));
        if (!n) return;
        entries = n;
        entry_cap = ncap;
    }
    Entry *e = &entries[entry_count++];
    strncpy(e->name, name, sizeof(e->name) - 1);
    e->name[sizeof(e->name) - 1] = 0;
    e->type = type;
    e->size = size;
}

static void list_dir(const char *path, const char *focus) {
    entry_count = 0;
    if (path[0] == 0) {
        static const char *const drives[] = {"ux0:", "uma0:", "ur0:", "imc0:", "xmc0:", "grw0:"};
        for (unsigned i = 0; i < sizeof(drives) / sizeof(drives[0]); i++) {
            SceUID d = sceIoDopen(drives[i]);
            if (d >= 0) {
                sceIoDclose(d);
                push_entry(drives[i], ENTRY_DRIVE, 0);
            }
        }
    } else {
        SceUID d = sceIoDopen(path);
        if (d >= 0) {
            SceIoDirent ent;
            memset(&ent, 0, sizeof(ent));
            while (sceIoDread(d, &ent) > 0) {
                if (ent.d_name[0] == '.') {
                    memset(&ent, 0, sizeof(ent));
                    continue;
                }
                if (SCE_S_ISDIR(ent.d_stat.st_mode)) {
                    push_entry(ent.d_name, ENTRY_DIR, 0);
                } else if (has_ext(ent.d_name, ".vb") || has_ext(ent.d_name, ".zip")) {
                    push_entry(ent.d_name, ENTRY_ROM, ent.d_stat.st_size);
                }
                memset(&ent, 0, sizeof(ent));
            }
            sceIoDclose(d);
        }
        qsort(entries, entry_count, sizeof(Entry), entry_cmp);
    }
    browser_sel = 0;
    if (focus) {
        for (int i = 0; i < entry_count; i++) {
            if (strcmp(entries[i].name, focus) == 0) {
                browser_sel = i;
                break;
            }
        }
    }
    browser_scroll = browser_sel - LIST_ROWS / 2;
    if (browser_scroll < 0) browser_scroll = 0;
    browser_hl = browser_sel;
    browser_sel_time = ui_time();
}

static void join_path(char *out, size_t len, const char *dir, const char *name) {
    size_t n = strlen(dir);
    if (n == 0) snprintf(out, len, "%s", name);
    else if (dir[n - 1] == ':' || dir[n - 1] == '/') snprintf(out, len, "%s%s", dir, name);
    else snprintf(out, len, "%s/%s", dir, name);
}

static bool dir_exists(const char *path) {
    SceUID d = sceIoDopen(path);
    if (d < 0) return false;
    sceIoDclose(d);
    return true;
}

static void go_parent(void) {
    if (cur_path[0] == 0) return;
    char focus[256];
    size_t n = strlen(cur_path);
    char *slash = strrchr(cur_path, '/');
    char *colon = strchr(cur_path, ':');
    if (cur_path[n - 1] == ':') {
        snprintf(focus, sizeof(focus), "%s", cur_path);
        cur_path[0] = 0;
    } else if (slash) {
        snprintf(focus, sizeof(focus), "%s", slash + 1);
        if (colon && slash == colon + 1) slash[1] = 0;
        else *slash = 0;
    } else if (colon) {
        snprintf(focus, sizeof(focus), "%s", colon + 1);
        colon[1] = 0;
    } else {
        snprintf(focus, sizeof(focus), "%s", cur_path);
        cur_path[0] = 0;
    }
    list_dir(cur_path, focus);
}

void menu_init(void) {
    thumb_tex = ui_texture_create(THUMB_W, THUMB_H, NULL);
    load_art_index();
    art_name[0] = 0;
    memset(&dialog, 0, sizeof(dialog));
    strncpy(cur_path, settings.last_dir, sizeof(cur_path) - 1);
    cur_path[sizeof(cur_path) - 1] = 0;
    if (cur_path[0] && !dir_exists(cur_path)) cur_path[0] = 0;
    list_dir(cur_path, NULL);
}

void menu_open_browser(void) {
    screen = SCREEN_BROWSER;
    screen_born = ui_time();
    char focus[256] = {0};
    const char *slash = strrchr(selected_path, '/');
    if (slash) {
        strncpy(focus, slash + 1, sizeof(focus) - 1);
        focus[sizeof(focus) - 1] = 0;
    }
    list_dir(cur_path, focus[0] ? focus : NULL);
}

void menu_open_pause(const char *game_name) {
    screen = SCREEN_PAUSE;
    screen_born = ui_time();
    pause_sel = PI_RESUME;
    pause_hl = 0;
    strncpy(pause_game, game_name, sizeof(pause_game) - 1);
    pause_game[sizeof(pause_game) - 1] = 0;
    thumb_loaded_slot = -2;
}

MenuScreen menu_screen(void) {
    return screen;
}

const char *menu_selected_path(void) {
    return selected_path;
}

int menu_state_slot(void) {
    return state_slot;
}

bool slot_thumbnail_path(int slot, char *out, int len) {
    char *p = get_savestate_path(slot, true);
    if (!p) return false;
    char *dot = strrchr(p, '.');
    if (dot) *dot = 0;
    snprintf(out, len, "%s.thm", p);
    free(p);
    return true;
}

static void load_thumb(int slot) {
    thumb_loaded_slot = slot;
    thumb_valid = false;
    thumb_time[0] = 0;
    char *sp = get_savestate_path(slot, false);
    if (!sp) return;
    SceIoStat st;
    if (sceIoGetstat(sp, &st) < 0) {
        free(sp);
        return;
    }
    SceRtcTick utc, local;
    SceDateTime dt;
    sceRtcGetTick(&st.st_mtime, &utc);
    sceRtcConvertUtcToLocalTime(&utc, &local);
    sceRtcSetTick(&dt, &local);
    snprintf(thumb_time, sizeof(thumb_time), "%04d-%02d-%02d  %02d:%02d", dt.year, dt.month, dt.day, dt.hour, dt.minute);
    free(sp);
    char tp[512];
    if (!slot_thumbnail_path(slot, tp, sizeof(tp))) return;
    FILE *f = fopen(tp, "rb");
    if (!f) return;
    uint32_t hdr[3];
    if (fread(hdr, 4, 3, f) == 3 && hdr[0] == 0x48545652 && hdr[1] == THUMB_W && hdr[2] == THUMB_H) {
        bool ok = fread(thumb_pixels, 4, THUMB_W * THUMB_H, f) == THUMB_W * THUMB_H;
        if (ok) ui_texture_update(thumb_tex, thumb_pixels);
        thumb_valid = ok;
    }
    fclose(f);
}

void menu_state_changed(int slot) {
    if (slot == thumb_loaded_slot) thumb_loaded_slot = -2;
}

void menu_show_message(const char *title, const char *body) {
    memset(&dialog, 0, sizeof(dialog));
    dialog.active = true;
    dialog.is_confirm = false;
    strncpy(dialog.title, title, sizeof(dialog.title) - 1);
    strncpy(dialog.body, body, sizeof(dialog.body) - 1);
    dialog.born = ui_time();
}

static void show_confirm(const char *title, const char *body, MenuAction on_yes) {
    memset(&dialog, 0, sizeof(dialog));
    dialog.active = true;
    dialog.is_confirm = true;
    strncpy(dialog.title, title, sizeof(dialog.title) - 1);
    strncpy(dialog.body, body, sizeof(dialog.body) - 1);
    dialog.on_yes = on_yes;
    dialog.choice = 1;
    dialog.born = ui_time();
}

static MenuAction dialog_update(const InputFrame *in) {
    if (dialog.is_confirm) {
        if (in->repeat & (BTN_LEFT | BTN_RIGHT)) dialog.choice ^= 1;
        if (in->pressed & BTN_BACK) {
            dialog.active = false;
            return ACT_NONE;
        }
        if (in->pressed & BTN_CONFIRM) {
            dialog.active = false;
            return dialog.choice == 0 ? dialog.on_yes : ACT_NONE;
        }
    } else if (in->pressed & (BTN_CONFIRM | BTN_BACK)) {
        dialog.active = false;
    }
    return ACT_NONE;
}

static int wrap_lines(float scale, float maxw, const char *text, char lines[][160], int max_lines) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", text);
    int n = 0;
    char line[160] = "";
    for (char *tok = strtok(buf, " "); tok; tok = strtok(NULL, " ")) {
        char trial[320];
        if (line[0]) snprintf(trial, sizeof(trial), "%s %s", line, tok);
        else snprintf(trial, sizeof(trial), "%s", tok);
        if (line[0] && ui_text_width(scale, trial) > maxw) {
            if (n < max_lines) snprintf(lines[n], 160, "%s", line);
            n++;
            snprintf(line, sizeof(line), "%s", tok);
        } else {
            snprintf(line, sizeof(line), "%s", trial);
        }
    }
    if (line[0]) {
        if (n < max_lines) snprintf(lines[n], 160, "%s", line);
        n++;
    }
    return n;
}

static void draw_wrapped(float x, float y, float maxw, float maxh, const char *text) {
    static char lines[24][160];
    float scale = 0.78f;
    int n = 0;
    float lh = 22;
    for (; scale > 0.55f; scale -= 0.04f) {
        lh = 28.0f * scale;
        n = wrap_lines(scale, maxw, text, lines, 24);
        if (n * lh <= maxh) break;
    }
    if (n > 24) n = 24;
    for (int i = 0; i < n && (i + 1) * lh <= maxh + lh * 0.5f; i++) ui_text(x, y + i * lh, COL_TEXT_DIM, scale, lines[i]);
}

static void open_settings(MenuScreen from) {
    settings_return = from;
    screen = SCREEN_SETTINGS;
    screen_born = ui_time();
    settings_hl = settings_sel;
}


static void palette_binding(int **value, int *count, const char *const **names) {
    *value = &settings.tint;
    *count = COLOR_TINT_COUNT;
    *names = color_tint_names;
}

static void on_setting_changed(bool game_loaded, const SettingRow *r) {
    (void)r;
    settings_apply();
    if (game_loaded) video_recompose();
}

static MenuAction browser_update(const InputFrame *in) {
    if (in->repeat & BTN_UP) browser_sel--;
    if (in->repeat & BTN_DOWN) browser_sel++;
    if (in->repeat & BTN_L) browser_sel -= LIST_ROWS;
    if (in->repeat & BTN_R) browser_sel += LIST_ROWS;
    if (in->repeat & (BTN_UP | BTN_DOWN | BTN_L | BTN_R)) {
        if (entry_count > 0) {
            if (browser_sel < 0) browser_sel = (in->repeat & BTN_UP) && !(in->held & BTN_L) ? entry_count - 1 : 0;
            if (browser_sel >= entry_count) browser_sel = (in->repeat & BTN_DOWN) && !(in->held & BTN_R) ? 0 : entry_count - 1;
        } else {
            browser_sel = 0;
        }
        browser_sel_time = ui_time();
    }
    if (in->pressed & BTN_TRIANGLE) {
        open_settings(SCREEN_BROWSER);
        return ACT_NONE;
    }
    if (in->pressed & BTN_BACK) {
        go_parent();
        return ACT_NONE;
    }
    if (in->pressed & BTN_START) {
        show_confirm("Exit Red Viper?", "Return to the LiveArea.", ACT_QUIT);
        return ACT_NONE;
    }
    if ((in->pressed & BTN_CONFIRM) && entry_count > 0) {
        Entry *e = &entries[browser_sel];
        if (e->type == ENTRY_DRIVE || e->type == ENTRY_DIR) {
            char np[512];
            join_path(np, sizeof(np), cur_path, e->name);
            strncpy(cur_path, np, sizeof(cur_path) - 1);
            cur_path[sizeof(cur_path) - 1] = 0;
            list_dir(cur_path, NULL);
        } else {
            join_path(selected_path, sizeof(selected_path), cur_path, e->name);
            strncpy(settings.last_dir, cur_path, sizeof(settings.last_dir) - 1);
            settings.last_dir[sizeof(settings.last_dir) - 1] = 0;
            settings_save();
            return ACT_LAUNCH;
        }
    }
    return ACT_NONE;
}

static MenuAction pause_update(const InputFrame *in) {
    if (in->repeat & BTN_UP) pause_sel = (pause_sel + PI_COUNT - 1) % PI_COUNT;
    if (in->repeat & BTN_DOWN) pause_sel = (pause_sel + 1) % PI_COUNT;
    int id = pause_items[pause_sel].id;
    bool slot_item = id == PI_SAVE || id == PI_LOAD;
    if (in->repeat & (BTN_LEFT | BTN_L)) {
        if (slot_item || (in->repeat & BTN_L)) {
            state_slot = (state_slot + STATE_SLOTS - 1) % STATE_SLOTS;
            slot_anim = -1;
        }
    }
    if (in->repeat & (BTN_RIGHT | BTN_R)) {
        if (slot_item || (in->repeat & BTN_R)) {
            state_slot = (state_slot + 1) % STATE_SLOTS;
            slot_anim = 1;
        }
    }
    if (in->pressed & BTN_BACK) return ACT_RESUME;
    if (in->pressed & BTN_CONFIRM) {
        switch (id) {
            case PI_RESUME:
                return ACT_RESUME;
            case PI_SAVE:
                if (emulation_hasstate(state_slot)) {
                    char body[96];
                    snprintf(body, sizeof(body), "Slot %d already has a save. Overwrite it?", state_slot + 1);
                    show_confirm("Overwrite state?", body, ACT_SAVE_STATE);
                    return ACT_NONE;
                }
                return ACT_SAVE_STATE;
            case PI_LOAD:
                if (!emulation_hasstate(state_slot)) {
                    ui_toast("That slot is empty");
                    return ACT_NONE;
                }
                return ACT_LOAD_STATE;
            case PI_FF:
                input_set_fast_forward_latch(!input_get_fast_forward_latch());
                return ACT_NONE;
            case PI_RESET:
                show_confirm("Reset the game?", "Unsaved progress since the last save will be lost.", ACT_RESET);
                return ACT_NONE;
            case PI_SETTINGS:
                open_settings(SCREEN_PAUSE);
                return ACT_NONE;
            case PI_CLOSE:
                show_confirm("Close the game?", "Battery saves are written automatically.", ACT_CLOSE);
                return ACT_NONE;
        }
    }
    return ACT_NONE;
}

static MenuAction settings_update(const InputFrame *in, bool game_loaded) {
    int dir = 0;
    if (in->repeat & BTN_UP) dir = -1;
    if (in->repeat & BTN_DOWN) dir = 1;
    if (dir) {
        int n = settings_sel;
        do {
            n = (n + dir + ROW_COUNT) % ROW_COUNT;
        } while (rows[n].kind == ROW_HEADER);
        settings_sel = n;
    }
    SettingRow *r = &rows[settings_sel];
    int delta = 0;
    if (in->repeat & BTN_LEFT) delta = -1;
    if (in->repeat & BTN_RIGHT) delta = 1;
    if ((in->pressed & BTN_CONFIRM) && (r->kind == ROW_CHOICE || r->kind == ROW_PALETTE)) delta = 1;
    if (r->kind == ROW_ACTION && (in->pressed & BTN_CONFIRM)) {
        char keep[512];
        strncpy(keep, settings.last_dir, sizeof(keep));
        settings_defaults();
        strncpy(settings.last_dir, keep, sizeof(settings.last_dir));
        on_setting_changed(game_loaded, NULL);
        ui_toast("Settings restored to defaults");
    } else if (delta && r->kind == ROW_PALETTE) {
        int *value, count;
        const char *const *names;
        palette_binding(&value, &count, &names);
        *value = (*value + delta + count) % count;
        on_setting_changed(game_loaded, r);
    } else if (delta && r->value) {
        int count = r->count;
        if (r->kind == ROW_SLIDER) *r->value = clampi(*r->value + delta, 0, count - 1);
        else *r->value = (*r->value + delta + count) % count;
        on_setting_changed(game_loaded, r);
    }
    if (in->pressed & (BTN_BACK | BTN_TRIANGLE)) {
        settings_save();
        screen = settings_return;
        screen_born = ui_time();
        if (screen == SCREEN_PAUSE) thumb_loaded_slot = -2;
    }
    return ACT_NONE;
}

MenuAction menu_update(const InputFrame *in, bool game_loaded) {
    if (dialog.active) return dialog_update(in);
    switch (screen) {
        case SCREEN_BROWSER:
            return browser_update(in);
        case SCREEN_PAUSE:
            return pause_update(in);
        case SCREEN_SETTINGS:
            return settings_update(in, game_loaded);
    }
    return ACT_NONE;
}

static void format_size(char *out, size_t len, int64_t size) {
    if (size >= 1024 * 1024) snprintf(out, len, "%.1f MB", size / (1024.0 * 1024.0));
    else snprintf(out, len, "%d KB", (int)((size + 1023) / 1024));
}

static void strip_ext(char *out, size_t len, const char *name) {
    snprintf(out, len, "%s", name);
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = 0;
}

static void draw_selection_bar(float x, float y, float w, float h, float glow) {
    ui_round_rect(x, y, w, h, 10, RGBA8(255, 38, 64, (int)(40 + 30 * glow)));
    ui_gradient_h(x, y + 2, w * 0.7f, h - 4, RGBA8(255, 38, 64, 90), RGBA8(255, 38, 64, 0));
    ui_round_rect(x + 6, y + 9, 4, h - 18, 2, COL_ACCENT);
    ui_round_outline(x, y, w, h, 10, RGBA8(255, 80, 100, (int)(90 + 60 * glow)));
}

static void art_key(char *out, size_t len, const char *name, bool base_only) {
    size_t n = 0;
    int depth = 0;
    bool pending_sep = false;
    for (const char *p = name; *p && n + 1 < len; p++) {
        unsigned char c = (unsigned char)*p;
        if (base_only && (c == '(' || c == '[')) {
            depth++;
            continue;
        }
        if (base_only && (c == ')' || c == ']')) {
            if (depth) depth--;
            continue;
        }
        if (depth) continue;
        if (isalnum(c)) {
            if (pending_sep && n > 0 && n + 2 < len) out[n++] = '_';
            out[n++] = (char)tolower(c);
            pending_sep = false;
        } else {
            pending_sep = true;
        }
    }
    out[n] = 0;
}

static void load_art_index(void) {
    FILE *f = fopen(ART_DIR "/index.txt", "r");
    if (!f) return;
    char line[256];
    int cap = 0;
    while (fgets(line, sizeof(line), f)) {
        char k[96], b[96];
        if (sscanf(line, "%95s %95s", k, b) != 2) continue;
        if (art_index_count >= cap) {
            cap = cap ? cap * 2 : 32;
            ArtIndex *n = realloc(art_index, cap * sizeof(ArtIndex));
            if (!n) break;
            art_index = n;
        }
        snprintf(art_index[art_index_count].key, sizeof(art_index[0].key), "%s", k);
        snprintf(art_index[art_index_count].base, sizeof(art_index[0].base), "%s", b);
        art_index_count++;
    }
    fclose(f);
}

static bool file_exists(const char *path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

static UiTexture *find_art(const char *rom_label) {
    char path[640];
    snprintf(path, sizeof(path), USER_ART_DIR "/%s.png", rom_label);
    if (file_exists(path)) return ui_texture_load(path);
    snprintf(path, sizeof(path), USER_ART_DIR "/%s.jpg", rom_label);
    if (file_exists(path)) return ui_texture_load(path);
    char key[256], base[256];
    art_key(key, sizeof(key), rom_label, false);
    art_key(base, sizeof(base), rom_label, true);
    for (int i = 0; i < art_index_count; i++) {
        if (strcmp(art_index[i].key, key) == 0) {
            snprintf(path, sizeof(path), ART_DIR "/%s.png", art_index[i].key);
            return ui_texture_load(path);
        }
    }
    for (int i = 0; i < art_index_count; i++) {
        if (strcmp(art_index[i].base, base) == 0) {
            snprintf(path, sizeof(path), ART_DIR "/%s.png", art_index[i].key);
            return ui_texture_load(path);
        }
    }
    return NULL;
}

static void update_art(const char *rom_name, const char *rom_label) {
    if (strcmp(art_name, rom_name) == 0) return;
    if (ui_time() - browser_sel_time < 0.12f) return;
    if (art_tex) {
        ui_texture_free(art_tex);
        art_tex = NULL;
    }
    snprintf(art_name, sizeof(art_name), "%s", rom_name);
    art_tex = find_art(rom_label);
    art_born = ui_time();
}

static void draw_art_box(float x, float y, float w, float h, const char *rom_name) {
    ui_round_rect(x, y, w, h, 12, RGBA8(0, 0, 0, 120));
    bool have = art_tex && strcmp(art_name, rom_name) == 0;
    if (have) {
        float tw = art_tex->w, th = art_tex->h;
        float sc = (w - 16) / tw < (h - 16) / th ? (w - 16) / tw : (h - 16) / th;
        float dw = tw * sc, dh = th * sc;
        float a = ui_ease_out((ui_time() - art_born) / 0.25f);
        ui_draw_texture(art_tex, x + (w - dw) / 2, y + (h - dh) / 2 + (1 - a) * 6, dw, dh, ui_alpha(0xFFFFFFFF, a));
    } else {
        ui_icon_file(x + w / 2 - 27, y + h / 2 - 36, 3.0f, RGBA8(255, 38, 64, 170));
    }
}

static void draw_browser(void) {
    ui_header("Game library");
    float enter = ui_ease_out((ui_time() - screen_born) / 0.35f);
    float off = (1 - enter) * 30;

    char crumb[600];
    if (cur_path[0]) snprintf(crumb, sizeof(crumb), "%s", cur_path);
    else snprintf(crumb, sizeof(crumb), "Storage devices");
    ui_text_clipped(LIST_X + 8, 72, LIST_W - 16, COL_TEXT_FAINT, 0.72f, crumb, ui_time());

    ui_round_rect(LIST_X - off, LIST_Y, LIST_W, LIST_ROWS * ROW_H + 16, 16, ui_alpha(COL_PANEL, enter));
    ui_round_outline(LIST_X - off, LIST_Y, LIST_W, LIST_ROWS * ROW_H + 16, 16, ui_alpha(COL_BORDER, enter));

    float target_scroll = browser_scroll;
    if (browser_sel < target_scroll + 1) target_scroll = browser_sel - 1;
    if (browser_sel > target_scroll + LIST_ROWS - 2) target_scroll = browser_sel - LIST_ROWS + 2;
    float max_scroll = entry_count - LIST_ROWS;
    if (max_scroll < 0) max_scroll = 0;
    if (target_scroll > max_scroll) target_scroll = max_scroll;
    if (target_scroll < 0) target_scroll = 0;
    browser_scroll = ui_approach(browser_scroll, target_scroll, 18);
    browser_hl = ui_approach(browser_hl, browser_sel, 22);

    float top = LIST_Y + 8;
    ui_clip_push(LIST_X, top, LIST_X + LIST_W, top + LIST_ROWS * ROW_H);
    if (entry_count == 0) {
        ui_text_center(LIST_X + LIST_W / 2, top + 150, COL_TEXT_DIM, 1.0f, cur_path[0] ? "No Virtual Boy games here" : "No storage devices found");
        ui_text_center(LIST_X + LIST_W / 2, top + 182, COL_TEXT_FAINT, 0.78f, "Copy .vb or .zip files to " RV_ROM_DIR);
    } else {
        float hy = top + (browser_hl - browser_scroll) * ROW_H;
        float glow = 0.5f + 0.5f * sinf(ui_time() * 3.0f);
        draw_selection_bar(LIST_X + 8 - off, hy + 2, LIST_W - 26, ROW_H - 4, glow);
        int first = (int)floorf(browser_scroll);
        for (int i = first; i < entry_count && i < first + LIST_ROWS + 2; i++) {
            if (i < 0) continue;
            float y = top + (i - browser_scroll) * ROW_H;
            Entry *e = &entries[i];
            bool sel = i == browser_sel;
            uint32_t ic = sel ? COL_ACCENT : COL_TEXT_DIM;
            float ix = LIST_X + 24 - off;
            if (e->type == ENTRY_DIR) ui_icon_folder(ix, y + 9, ic);
            else if (e->type == ENTRY_DRIVE) ui_icon_drive(ix, y + 9, ic);
            else ui_icon_file(ix + 3, y + 9, 1.0f, ic);
            char label[256];
            if (e->type == ENTRY_ROM) strip_ext(label, sizeof(label), e->name);
            else snprintf(label, sizeof(label), "%s", e->name);
            float tx = LIST_X + 62 - off;
            float maxw = LIST_W - 62 - 110;
            if (sel) ui_text_clipped(tx, y + 11, maxw, COL_TEXT, 0.95f, label, ui_time() - browser_sel_time);
            else ui_text_clipped(tx, y + 11, maxw, COL_TEXT_DIM, 0.95f, label, 0);
            if (e->type == ENTRY_ROM) {
                char sz[32];
                format_size(sz, sizeof(sz), e->size);
                ui_text_right(LIST_X + LIST_W - 30 - off, y + 14, COL_TEXT_FAINT, 0.72f, sz);
            } else {
                float ax = LIST_X + LIST_W - 38 - off, ay = y + ROW_H / 2;
                ui_thick_line(ax, ay - 5, ax + 5, ay, 2, ui_alpha(ic, 0.8f));
                ui_thick_line(ax + 5, ay, ax, ay + 5, 2, ui_alpha(ic, 0.8f));
            }
        }
    }
    ui_clip_pop();

    if (entry_count > LIST_ROWS) {
        float track_h = LIST_ROWS * ROW_H - 8;
        float bar_h = track_h * LIST_ROWS / entry_count;
        if (bar_h < 24) bar_h = 24;
        float max_scroll2 = entry_count - LIST_ROWS;
        float bar_y = top + 4 + (track_h - bar_h) * (browser_scroll / max_scroll2);
        ui_round_rect(LIST_X + LIST_W - 12, top + 4, 4, track_h, 2, RGBA8(255, 255, 255, 18));
        ui_round_rect(LIST_X + LIST_W - 12, bar_y, 4, bar_h, 2, RGBA8(255, 38, 64, 200));
    }

    float px = 620 + off;
    float pw = SCREEN_W - 32 - px;
    ui_round_rect(px, LIST_Y, pw, LIST_ROWS * ROW_H + 16, 16, ui_alpha(COL_PANEL, enter));
    ui_round_outline(px, LIST_Y, pw, LIST_ROWS * ROW_H + 16, 16, ui_alpha(COL_BORDER, enter));
    if (entry_count > 0) {
        Entry *e = &entries[browser_sel];
        if (e->type == ENTRY_ROM) {
            char label[256];
            strip_ext(label, sizeof(label), e->name);
            update_art(e->name, label);
            draw_art_box(px + 10, LIST_Y + 10, pw - 20, 240, e->name);
            ui_text_clipped(px + 20, LIST_Y + 272, pw - 40, COL_TEXT, 1.0f, label, ui_time() - browser_sel_time);
            char info[64], sz[32];
            format_size(sz, sizeof(sz), e->size);
            snprintf(info, sizeof(info), "%s  ·  %s", has_ext(e->name, ".zip") ? "ZIP archive" : "VB ROM", sz);
            ui_text(px + 20, LIST_Y + 302, COL_TEXT_DIM, 0.78f, info);
            ui_glyph(GLYPH_CONFIRM, px + 20, LIST_Y + 352, 1.0f);
            ui_text(px + 50, LIST_Y + 342, COL_TEXT, 0.85f, "Play");
        } else {
            float cx = px + pw / 2;
            float cy = LIST_Y + 130;
            float s = 3.2f;
            uint32_t c = RGBA8(255, 38, 64, 200);
            if (e->type == ENTRY_DRIVE) {
                ui_round_rect(cx - 12 * s, cy - 7 * s, 24 * s, 14 * s, 3 * s, c);
                ui_circle(cx + 6 * s, cy, 2 * s, RGBA8(0, 0, 0, 120));
            } else {
                ui_round_rect(cx - 12 * s, cy - 9 * s, 12 * s, 6 * s, 2 * s, c);
                ui_round_rect(cx - 12 * s, cy - 6 * s, 24 * s, 16 * s, 3 * s, c);
            }
            ui_text_center(cx, LIST_Y + 220, COL_TEXT, 1.0f, e->name);
            ui_text_center(cx, LIST_Y + 252, COL_TEXT_DIM, 0.78f, e->type == ENTRY_DRIVE ? "Storage device" : "Folder");
        }
    } else {
        ui_text_center(px + pw / 2, LIST_Y + 180, COL_TEXT_FAINT, 0.85f, "Nothing selected");
    }

    UiHint hints[] = {
        {GLYPH_CONFIRM, "Open"},
        {GLYPH_BACK, "Up"},
        {GLYPH_TRIANGLE, "Settings"},
        {GLYPH_START, "Exit"},
    };
    ui_hints(hints, 4);
}

static void draw_pause(void) {
    float enter = ui_ease_out((ui_time() - screen_born) / 0.3f);
    video_present_game(0.6f * enter);
    ui_gradient_h(0, 0, 640, SCREEN_H, RGBA8(6, 0, 3, (int)(235 * enter)), RGBA8(6, 0, 3, 0));
    ui_header(NULL);

    float panel_x = -360 + 392 * enter;
    ui_text_clipped(panel_x, 84, 330, COL_TEXT_DIM, 0.85f, pause_game, ui_time());
    pause_hl = ui_approach(pause_hl, pause_sel, 22);
    float top = 122;
    float rh = 48;
    float glow = 0.5f + 0.5f * sinf(ui_time() * 3.0f);
    draw_selection_bar(panel_x - 8, top + pause_hl * rh + 2, 340, rh - 4, glow);
    for (int i = 0; i < PI_COUNT; i++) {
        float y = top + i * rh;
        bool sel = i == pause_sel;
        float slide = sel ? 10 : 0;
        uint32_t c = sel ? COL_TEXT : COL_TEXT_DIM;
        ui_text(panel_x + 14 + slide, y + 13, c, 1.05f, pause_items[i].label);
        int id = pause_items[i].id;
        if (id == PI_SAVE || id == PI_LOAD) {
            char s[24];
            snprintf(s, sizeof(s), "Slot %d", state_slot + 1);
            float rx = panel_x + 318;
            ui_text_right(rx - (sel ? 16 : 0), y + 15, sel ? COL_ACCENT : COL_TEXT_FAINT, 0.85f, s);
            if (sel) {
                float w = ui_text_width(0.85f, s);
                float ay = y + rh / 2;
                ui_thick_line(rx - w - 30, ay, rx - w - 24, ay - 6, 2, COL_ACCENT);
                ui_thick_line(rx - w - 30, ay, rx - w - 24, ay + 6, 2, COL_ACCENT);
                ui_thick_line(rx - 4, ay, rx - 10, ay - 6, 2, COL_ACCENT);
                ui_thick_line(rx - 4, ay, rx - 10, ay + 6, 2, COL_ACCENT);
            }
        } else if (id == PI_FF) {
            bool on = input_get_fast_forward_latch();
            float sx = panel_x + 270, sy = y + 14;
            ui_round_rect(sx, sy, 44, 22, 11, on ? COL_ACCENT : RGBA8(255, 255, 255, 40));
            ui_circle(on ? sx + 33 : sx + 11, sy + 11, 8, COL_TEXT);
        }
    }

    if (thumb_loaded_slot != state_slot) load_thumb(state_slot);
    slot_anim = ui_approach(slot_anim, 0, 14);
    float cw = 2 * THUMB_W + 24;
    float cx = SCREEN_W - 40 - cw + (1 - enter) * 60 + slot_anim * 24;
    float cy = 130;
    float ca = enter * (1 - fabsf(slot_anim) * 0.6f);
    ui_round_rect(cx + 6, cy + 8, cw, 2 * THUMB_H + 84, 18, ui_alpha(COL_SHADOW, ca));
    ui_round_rect(cx, cy, cw, 2 * THUMB_H + 84, 18, ui_alpha(COL_PANEL_SOLID, ca));
    ui_round_outline(cx, cy, cw, 2 * THUMB_H + 84, 18, ui_alpha(COL_BORDER, ca * 1.5f));
    float tx = cx + 12, ty = cy + 12;
    if (thumb_valid) {
        ui_draw_texture(thumb_tex, tx, ty, 2 * THUMB_W, 2 * THUMB_H, ui_alpha(0xFFFFFFFF, ca));
    } else {
        ui_round_rect(tx, ty, 2 * THUMB_W, 2 * THUMB_H, 8, ui_alpha(RGBA8(0, 0, 0, 255), ca));
        for (int i = 0; i < 2 * THUMB_H; i += 6)
            ui_rect(tx + 8, ty + i, 2 * THUMB_W - 16, 1, ui_alpha(RGBA8(255, 38, 64, 16), ca));
        ui_text_center(tx + THUMB_W, ty + THUMB_H - 12, ui_alpha(COL_TEXT_FAINT, ca), 0.9f, thumb_time[0] ? "No preview" : "Empty slot");
    }
    char slot_title[32];
    snprintf(slot_title, sizeof(slot_title), "Slot %d", state_slot + 1);
    ui_text(tx + 4, ty + 2 * THUMB_H + 18, ui_alpha(COL_TEXT, ca), 1.0f, slot_title);
    ui_text_right(tx + 2 * THUMB_W - 4, ty + 2 * THUMB_H + 21, ui_alpha(COL_TEXT_DIM, ca), 0.78f, thumb_time[0] ? thumb_time : "Never saved");
    for (int i = 0; i < STATE_SLOTS; i++) {
        float dx = cx + cw / 2 - (STATE_SLOTS - 1) * 9 + i * 18;
        float dy = cy + 2 * THUMB_H + 112;
        ui_circle(dx, dy, i == state_slot ? 5 : 3.5f, ui_alpha(i == state_slot ? COL_ACCENT : RGBA8(255, 255, 255, 60), enter));
    }

    UiHint hints[] = {
        {GLYPH_CONFIRM, "Select"},
        {GLYPH_BACK, "Resume"},
        {GLYPH_L, "Slot"},
        {GLYPH_R, "Slot"},
    };
    ui_hints(hints, 4);
}

static void draw_settings(bool game_loaded) {
    if (settings_return == SCREEN_PAUSE && game_loaded) {
        video_present_game(0.82f);
    } else {
        ui_background();
    }
    ui_header("Settings");
    float enter = ui_ease_out((ui_time() - screen_born) / 0.3f);
    float off = (1 - enter) * 30;
    const float sx = 32, sy = 84, sw = 560, rh = 38;
    const int visible = 10;
    ui_round_rect(sx - off, sy, sw, visible * rh + 16, 16, ui_alpha(COL_PANEL, enter));
    ui_round_outline(sx - off, sy, sw, visible * rh + 16, 16, ui_alpha(COL_BORDER, enter));
    float target = settings_scroll;
    if (settings_sel < target + 2) target = settings_sel - 2;
    if (settings_sel > target + visible - 2) target = settings_sel - visible + 2;
    if (target > ROW_COUNT - visible) target = ROW_COUNT - visible;
    if (target < 0) target = 0;
    settings_scroll = ui_approach(settings_scroll, target, 18);
    settings_hl = ui_approach(settings_hl, settings_sel, 22);
    float top = sy + 8;
    ui_clip_push(sx, top, sx + sw, top + visible * rh);
    float glow = 0.5f + 0.5f * sinf(ui_time() * 3.0f);
    draw_selection_bar(sx + 8 - off, top + (settings_hl - settings_scroll) * rh + 2, sw - 16, rh - 4, glow);
    for (int i = 0; i < ROW_COUNT; i++) {
        float y = top + (i - settings_scroll) * rh;
        if (y < top - rh || y > top + visible * rh) continue;
        SettingRow *r = &rows[i];
        bool sel = i == settings_sel;
        if (r->kind == ROW_HEADER) {
            ui_text(sx + 22 - off, y + 12, COL_ACCENT, 0.72f, r->label);
            float lw = ui_text_width(0.72f, r->label);
            ui_gradient_h(sx + 32 + lw - off, y + 21, sw - 70 - lw, 1, RGBA8(255, 38, 64, 90), RGBA8(255, 38, 64, 0));
            continue;
        }
        uint32_t c = sel ? COL_TEXT : COL_TEXT_DIM;
        ui_text(sx + 30 + (sel ? 6 : 0) - off, y + 9, c, 0.92f, r->label);
        float vx = sx + sw - 28 - off;
        if (r->kind == ROW_CHOICE || r->kind == ROW_PALETTE) {
            const char *v;
            float swatch_w = 0;
            if (r->kind == ROW_PALETTE) {
                int *value, count;
                const char *const *names;
                palette_binding(&value, &count, &names);
                v = names[clampi(*value, 0, count - 1)];
                float pal[4][3];
                color_preview_palette(pal);
                float tw = ui_text_width(0.85f, v);
                float sx0 = vx - (sel ? 18 : 0) - tw - 12 - 4 * 12;
                for (int k = 0; k < 4; k++)
                    ui_round_rect(sx0 + k * 12, y + 12, 10, 14, 3, RGBA8((int)(pal[k][0] * 255), (int)(pal[k][1] * 255), (int)(pal[k][2] * 255), 255));
                swatch_w = 4 * 12 + 12;
            } else {
                v = r->names[clampi(*r->value, 0, r->count - 1)];
            }
            if (r->kind == ROW_CHOICE && r->names == onoff_names) {
                bool on = *r->value != 0;
                ui_round_rect(vx - 44, y + 8, 44, 22, 11, on ? COL_ACCENT : RGBA8(255, 255, 255, 40));
                ui_circle(on ? vx - 11 : vx - 33, y + 19, 8, COL_TEXT);
            } else {
                ui_text_right(vx - (sel ? 18 : 0), y + 10, sel ? COL_TEXT : COL_TEXT_DIM, 0.85f, v);
                if (sel) {
                    float w = ui_text_width(0.85f, v) + swatch_w;
                    float ay = y + rh / 2;
                    ui_thick_line(vx - w - 32, ay, vx - w - 26, ay - 6, 2, COL_ACCENT);
                    ui_thick_line(vx - w - 32, ay, vx - w - 26, ay + 6, 2, COL_ACCENT);
                    ui_thick_line(vx, ay, vx - 6, ay - 6, 2, COL_ACCENT);
                    ui_thick_line(vx, ay, vx - 6, ay + 6, 2, COL_ACCENT);
                }
            }
        } else if (r->kind == ROW_SLIDER) {
            float bw = 160;
            float bx = vx - bw;
            float fill = bw * (*r->value) / (float)(r->count - 1);
            ui_round_rect(bx, y + 16, bw, 6, 3, RGBA8(255, 255, 255, 40));
            ui_round_rect(bx, y + 16, fill, 6, 3, COL_ACCENT);
            ui_circle(bx + fill, y + 19, sel ? 9 : 7, COL_TEXT);
            char num[8];
            snprintf(num, sizeof(num), "%d", *r->value);
            ui_text_right(bx - 14, y + 10, c, 0.85f, num);
        } else if (r->kind == ROW_ACTION) {
            ui_glyph(GLYPH_CONFIRM, vx - 22, y + rh / 2, sel ? 1.0f : 0.5f);
        }
    }
    ui_clip_pop();

    float px = 612 + off, pw = SCREEN_W - 32 - px;
    float ph = visible * rh + 16;
    ui_round_rect(px, sy, pw, ph, 16, ui_alpha(COL_PANEL, enter));
    ui_round_outline(px, sy, pw, ph, 16, ui_alpha(COL_BORDER, enter));
    float pvw = pw - 32, pvh = pvw * VB_H / VB_W;
    float pvx = px + 16, pvy = sy + 16;
    ui_round_rect(pvx - 2, pvy - 2, pvw + 4, pvh + 4, 6, RGBA8(0, 0, 0, 255));
    if (game_loaded) {
        video_draw_game_at(pvx, pvy, pvw, pvh, 0xFFFFFFFF);
    } else {
        float pal[4][3];
        color_preview_palette(pal);
        for (int i = 0; i < 4; i++) {
            float bw = pvw / 4;
            ui_rect(pvx + i * bw, pvy, bw, pvh, RGBA8((int)(pal[i][0] * 255), (int)(pal[i][1] * 255), (int)(pal[i][2] * 255), 255));
        }
        ui_dot_text(pvx + pvw / 2 - ui_dot_text_width(3.0f, "PREVIEW") / 2, pvy + pvh / 2 - 10, 3.0f, RGBA8(255, 255, 255, 160), "PREVIEW");
    }
    SettingRow *r = &rows[settings_sel];
    ui_text(px + 18, pvy + pvh + 18, COL_TEXT, 1.0f, r->label);
    if (r->desc) draw_wrapped(px + 18, pvy + pvh + 50, pw - 36, sy + ph - 14 - (pvy + pvh + 50), r->desc);

    UiHint hints[] = {
        {GLYPH_DPAD, "Change"},
        {GLYPH_BACK, "Done"},
    };
    ui_hints(hints, 2);
}

static void draw_dialog(void) {
    float a = ui_ease_out((ui_time() - dialog.born) / 0.22f);
    ui_rect(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, (int)(150 * a)));
    float w = 520, h = dialog.is_confirm ? 190 : 170;
    float x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2 + (1 - a) * 24;
    ui_round_rect(x + 6, y + 10, w, h, 20, ui_alpha(COL_SHADOW, a));
    ui_round_rect(x, y, w, h, 20, ui_alpha(COL_PANEL_SOLID, a));
    ui_gradient_v(x, y, w, 60, ui_alpha(RGBA8(255, 38, 64, 50), a), ui_alpha(RGBA8(255, 38, 64, 0), a));
    ui_round_outline(x, y, w, h, 20, ui_alpha(RGBA8(255, 60, 90, 110), a));
    ui_text(x + 28, y + 24, ui_alpha(COL_TEXT, a), 1.1f, dialog.title);
    ui_text(x + 28, y + 66, ui_alpha(COL_TEXT_DIM, a), 0.85f, dialog.body);
    if (dialog.is_confirm) {
        const char *labels[2] = {"Yes", "No"};
        for (int i = 0; i < 2; i++) {
            float bw = 150, bx = x + w - 28 - (2 - i) * (bw + 14) + 14, by = y + h - 68;
            bool sel = dialog.choice == i;
            ui_round_rect(bx, by, bw, 44, 12, ui_alpha(sel ? COL_ACCENT : RGBA8(255, 255, 255, 22), a));
            if (!sel) ui_round_outline(bx, by, bw, 44, 12, ui_alpha(RGBA8(255, 255, 255, 40), a));
            ui_text_center(bx + bw / 2, by + 12, ui_alpha(COL_TEXT, a), 0.95f, labels[i]);
        }
    } else {
        float bw = 150, bx = x + w - 28 - bw, by = y + h - 64;
        ui_round_rect(bx, by, bw, 42, 12, ui_alpha(COL_ACCENT, a));
        ui_text_center(bx + bw / 2, by + 11, ui_alpha(COL_TEXT, a), 0.95f, "OK");
    }
}

void menu_draw(bool game_loaded) {
    switch (screen) {
        case SCREEN_BROWSER:
            ui_background();
            draw_browser();
            break;
        case SCREEN_PAUSE:
            draw_pause();
            break;
        case SCREEN_SETTINGS:
            draw_settings(game_loaded);
            break;
    }
    if (dialog.active) draw_dialog();
    ui_draw_toast();
}

void menu_draw_loading(const char *name, int percent) {
    ui_background();
    ui_header(NULL);
    float cx = SCREEN_W / 2.0f;
    ui_spinner(cx, 230, 26, COL_ACCENT);
    ui_text_center(cx, 290, COL_TEXT, 1.05f, "Loading");
    ui_text_center(cx, 322, COL_TEXT_DIM, 0.85f, name);
    float bw = 420, bx = cx - bw / 2, by = 372;
    ui_round_rect(bx, by, bw, 8, 4, RGBA8(255, 255, 255, 30));
    ui_round_rect(bx, by, bw * clampi(percent, 0, 100) / 100.0f, 8, 4, COL_ACCENT);
}

void menu_draw_fps(float fps, float speed) {
    char buf[48];
    snprintf(buf, sizeof(buf), "%.1f FPS  %d%%", fps, (int)(speed + 0.5f));
    float w = ui_text_width(0.7f, buf) + 20;
    ui_round_rect(SCREEN_W - w - 12, 10, w, 26, 13, RGBA8(0, 0, 0, 150));
    ui_text(SCREEN_W - w - 2, 13, COL_TEXT, 0.7f, buf);
}
