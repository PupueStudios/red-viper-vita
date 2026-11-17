#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/common_dialog.h>
#include <psp2/message_dialog.h>
#include <vitaGL.h>

#include "drc_core.h"
#include "replay.h"
#include "v810_cpu.h"
#include "v810_mem.h"
#include "vb_dsp.h"
#include "vb_gui.h"
#include "vb_set.h"
#include "vb_sound.h"
#include "video_hard.h"
#include "vita.h"
#include "vita_color.h"
#include "vita_colorpack.h"
#include "vita_menu.h"
#include "vita_ui.h"

int _newlib_heap_size_user = 128 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = 1024 * 1024;

#define FRAME_US 20000
#define FF_PRESENT_US 16666

typedef enum {
    APP_MENU,
    APP_LOADING,
    APP_GAME,
} AppState;

static AppState app_state = APP_MENU;
static bool game_loaded = false;
static char game_name[256];
static uint64_t next_frame_us = 0;
static uint64_t last_present_us = 0;
static uint64_t fps_window_us = 0;
static int fps_frames = 0;
static float fps_value = 0;
static int load_percent = 0;

static void ensure_dirs(void) {
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(RV_DATA_DIR, 0777);
    sceIoMkdir(RV_ROM_DIR, 0777);
    sceIoMkdir(RV_DATA_DIR "/saves", 0777);
    sceIoMkdir(RV_DATA_DIR "/savestates", 0777);
}

static void base_name(char *out, size_t len, const char *path) {
    const char *slash = strrchr(path, '/');
    const char *colon = strrchr(path, ':');
    const char *start = slash ? slash + 1 : colon ? colon + 1 : path;
    snprintf(out, len, "%s", start);
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = 0;
}

static bool file_exists(const char *path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

static void fatal_dialog(const char *text) {
    SceMsgDialogUserMessageParam msg;
    SceMsgDialogParam param;
    memset(&msg, 0, sizeof(msg));
    msg.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
    msg.msg = (const SceChar8 *)text;
    sceMsgDialogParamInit(&param);
    param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    param.userMsgParam = &msg;
    if (sceMsgDialogInit(&param) < 0) return;
    while (sceMsgDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_RUNNING) {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        vglSwapBuffers(GL_TRUE);
    }
    sceMsgDialogTerm();
}

static void enter_menu_mode(void) {
    vglWaitVblankStart(GL_TRUE);
    input_capture_ps_button(false);
}

static void enter_game_mode(void) {
    vglWaitVblankStart(GL_FALSE);
    next_frame_us = sceKernelGetProcessTimeWide() + FRAME_US;
    fps_window_us = sceKernelGetProcessTimeWide();
    fps_frames = 0;
    app_state = APP_GAME;
    input_capture_ps_button(true);
    sound_resume();
}

static void close_game(void) {
    if (!game_loaded) return;
    sound_pause();
    save_sram();
    game_loaded = false;
    cp_unload();
    tVBOpt.FASTFORWARD = 0;
    input_set_fast_forward_latch(false);
}

static bool begin_load(const char *path) {
    close_game();
    snprintf(tVBOpt.ROM_PATH, sizeof(tVBOpt.ROM_PATH), "%s", path);
    base_name(game_name, sizeof(game_name), path);
    snprintf(tVBOpt.RAM_PATH, sizeof(tVBOpt.RAM_PATH), "%s/saves/%s.ram", RV_DATA_DIR, game_name);
    int err = v810_load_init();
    if (err) {
        char body[128];
        snprintf(body, sizeof(body), "The file could not be opened (error %d).", err);
        menu_show_message("Unable to load game", body);
        tVBOpt.ROM_PATH[0] = 0;
        return false;
    }
    load_percent = 0;
    app_state = APP_LOADING;
    return true;
}

static void finish_load(void) {
    bool packed = cp_load_for_game(tVBOpt.ROM_PATH, game_name, (uint32_t)tVBOpt.CRC32, (uint32_t)V810_ROM1.size);
    video_color_refresh(false);
    clearCache();
    sound_reset();
    sound_pause();
    settings_apply();
    video_clear();
    game_loaded = true;
    tVBOpt.FASTFORWARD = 0;
    input_set_fast_forward_latch(false);
    enter_game_mode();
    if (packed && settings.color_mode != COLOR_MODE_TINT) ui_toast("Colour pack loaded");
}

static void step_loading(void) {
    int ret = v810_load_step();
    if (ret < 0) {
        char body[128];
        snprintf(body, sizeof(body), "Reading the ROM failed (error %d).", ret);
        menu_show_message("Unable to load game", body);
        tVBOpt.ROM_PATH[0] = 0;
        app_state = APP_MENU;
        menu_open_browser();
        return;
    }
    load_percent = ret;
    if (ret == 100) finish_load();
    ui_frame_begin();
    menu_draw_loading(game_name, load_percent);
    ui_frame_end();
}

static void write_thumbnail(int slot) {
    static uint32_t pixels[192 * 112];
    char path[512];
    if (!slot_thumbnail_path(slot, path, sizeof(path))) return;
    video_make_thumbnail(pixels, 192, 112);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t hdr[3] = {0x48545652, 192, 112};
    fwrite(hdr, 4, 3, f);
    fwrite(pixels, 4, 192 * 112, f);
    fclose(f);
}

static void open_pause(void) {
    sound_pause();
    save_sram();
    tVBOpt.FASTFORWARD = 0;
    menu_open_pause(game_name);
    enter_menu_mode();
    app_state = APP_MENU;
}

static bool handle_action(MenuAction act) {
    char msg[64];
    int slot = menu_state_slot();
    switch (act) {
        case ACT_LAUNCH:
            begin_load(menu_selected_path());
            break;
        case ACT_RESUME:
            if (game_loaded) enter_game_mode();
            break;
        case ACT_RESET:
            v810_reset();
            sound_reset();
            sound_pause();
            clearCache();
            video_clear();
            enter_game_mode();
            ui_toast("Game reset");
            break;
        case ACT_SAVE_STATE:
            if (emulation_sstate(slot) == 0) {
                write_thumbnail(slot);
                menu_state_changed(slot);
                snprintf(msg, sizeof(msg), "Saved to slot %d", slot + 1);
            } else {
                snprintf(msg, sizeof(msg), "Could not save slot %d", slot + 1);
            }
            ui_toast(msg);
            break;
        case ACT_LOAD_STATE:
            if (emulation_lstate(slot) == 0) {
                clearCache();
                video_render(vb_state->tVIPREG.tDisplayedFB % 2, false);
                snprintf(msg, sizeof(msg), "Loaded slot %d", slot + 1);
                ui_toast(msg);
                enter_game_mode();
            } else {
                snprintf(msg, sizeof(msg), "Slot %d is not compatible", slot + 1);
                ui_toast(msg);
            }
            break;
        case ACT_CLOSE:
            close_game();
            menu_open_browser();
            break;
        case ACT_QUIT:
            return false;
        default:
            break;
    }
    return true;
}

static void pace_frame(void) {
    uint64_t now = sceKernelGetProcessTimeWide();
    if (tVBOpt.FASTFORWARD) {
        next_frame_us = now + FRAME_US;
        return;
    }
    if (now < next_frame_us) {
        sceKernelDelayThread((SceUInt)(next_frame_us - now));
        next_frame_us += FRAME_US;
    } else if (now - next_frame_us > 100000) {
        next_frame_us = now + FRAME_US;
    } else {
        next_frame_us += FRAME_US;
    }
}

static bool run_game_frame(const InputFrame *in) {
    if (input_menu_combo(in)) {
        open_pause();
        return true;
    }
    uint16_t keys = input_game_blocked() ? 0 : input_vb_keys(in);
    vb_players[0].tHReg.SLB = keys & 0xFF;
    vb_players[0].tHReg.SHB = keys >> 8;
    tVBOpt.FASTFORWARD = input_fast_forward(in);

    int displayed_fb = vb_state->tVIPREG.tDisplayedFB;
    if (vb_state->tVIPREG.tFrame == 0 && !vb_state->tVIPREG.drawing && (vb_state->tVIPREG.DPCTRL & 0x0002)) {
        video_render(displayed_fb, true);
    }
    vb_state = &vb_players[0];
    int err = v810_run();
    if (err) {
        char body[128];
        snprintf(body, sizeof(body), "The CPU core stopped with error code %d.", err);
        close_game();
        enter_menu_mode();
        app_state = APP_MENU;
        menu_open_browser();
        menu_show_message("Emulation error", body);
        return true;
    }

    fps_frames++;
    uint64_t now = sceKernelGetProcessTimeWide();
    if (now - fps_window_us >= 1000000) {
        fps_value = fps_frames * 1000000.0f / (now - fps_window_us);
        fps_frames = 0;
        fps_window_us = now;
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
    }

    if (!tVBOpt.FASTFORWARD || now - last_present_us >= FF_PRESENT_US) {
        last_present_us = now;
        ui_frame_begin();
        video_present_game(0);
        if (settings.show_fps) menu_draw_fps(fps_value, fps_value * 100.0f / 50.0f);
        if (tVBOpt.FASTFORWARD) {
            ui_round_rect(16, 12, 64, 26, 13, RGBA8(0, 0, 0, 150));
            ui_thick_line(32, 18, 42, 25, 3, COL_ACCENT);
            ui_thick_line(42, 25, 32, 32, 3, COL_ACCENT);
            ui_thick_line(46, 18, 56, 25, 3, COL_ACCENT);
            ui_thick_line(56, 25, 46, 32, 3, COL_ACCENT);
        }
        ui_draw_toast();
        ui_frame_end();
    }
    pace_frame();
    return true;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    ensure_dirs();
    settings_load();

    vglInitExtended(0, SCREEN_W, SCREEN_H, 0x1000000, SCE_GXM_MULTISAMPLE_NONE);
    vglWaitVblankStart(GL_TRUE);

    if (!file_exists("ur0:data/libshacccg.suprx") && !file_exists("ur0:data/external/libshacccg.suprx")) {
        fatal_dialog("Red Viper needs the system shader compiler (libshacccg.suprx).\n\nExtract it with ShaRKF00D, then start Red Viper again.");
        sceKernelExitProcess(0);
        return 0;
    }

    if (!jit_probe()) {
        fatal_dialog("Dynarec memory is unavailable.\n\nInstall Red Viper as unsafe homebrew and enable Unsafe Homebrew in HENkaku settings.");
        sceKernelExitProcess(0);
        return 0;
    }

    ui_init();
    if (vita_shaders_failed()) {
        fatal_dialog("A graphics shader failed to compile.\n\nMake sure libshacccg.suprx is extracted correctly.");
        sceKernelExitProcess(0);
        return 0;
    }
    input_init();

    setDefaults();
    snprintf(tVBOpt.HOME_PATH, sizeof(tVBOpt.HOME_PATH), "%s", RV_DATA_DIR);
    v810_init();
    replay_init();
    drc_init();
    clearCache();
    audio_init();
    sound_init();
    sound_pause();
    video_init(true);
    video_init_vita();
    settings_apply();

    if (vita_shaders_failed()) {
        fatal_dialog("A graphics shader failed to compile.\n\nMake sure libshacccg.suprx is extracted correctly.");
        sceKernelExitProcess(0);
        return 0;
    }

    menu_init();
    menu_open_browser();
    enter_menu_mode();

    bool running = true;
    InputFrame in;
    while (running) {
        input_poll(&in);
        switch (app_state) {
            case APP_LOADING:
                step_loading();
                break;
            case APP_GAME:
                run_game_frame(&in);
                break;
            case APP_MENU: {
                MenuAction act = menu_update(&in, game_loaded);
                if (act != ACT_NONE) running = handle_action(act);
                if (app_state == APP_MENU) {
                    ui_frame_begin();
                    menu_draw(game_loaded);
                    ui_frame_end();
                }
                break;
            }
        }
    }

    close_game();
    input_release_ps_button_now();
    settings_save();
    audio_shutdown();
    drc_exit();
    video_shutdown_vita();
    video_quit();
    ui_shutdown();
    sceKernelExitProcess(0);
    return 0;
}
