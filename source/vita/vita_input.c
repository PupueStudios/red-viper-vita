#include <psp2/apputil.h>
#include <psp2/ctrl.h>
#include <psp2/shellutil.h>
#include <psp2/system_param.h>
#include <psp2/touch.h>
#include <string.h>

#include "vb_dsp.h"
#include "vita.h"

#define STICK_DEAD 56
#define REPEAT_DELAY 18
#define REPEAT_RATE 4

static SceCtrlData pad;
static SceTouchData touch_front;
static SceTouchData touch_back;
static uint32_t prev_held = 0;
static int repeat_timer[4];
static bool confirm_circle = false;
static bool prev_front_touch = false;
static bool ff_latched = false;
#define PS_LOCK_FLAGS (SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN | SCE_SHELL_UTIL_LOCK_TYPE_QUICK_MENU | SCE_SHELL_UTIL_LOCK_TYPE_USB_CONNECTION | SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN_2)

static bool ps_locked = false;
static bool ps_prev = false;
static bool ps_released = false;
static bool wait_all_released = false;

bool input_confirm_is_circle(void) {
    return confirm_circle;
}

void input_init(void) {
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
    SceAppUtilInitParam init;
    SceAppUtilBootParam boot;
    memset(&init, 0, sizeof(init));
    memset(&boot, 0, sizeof(boot));
    sceAppUtilInit(&init, &boot);
    sceShellUtilInitEvents(0);
    int enter = SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &enter);
    confirm_circle = enter == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE;
    memset(repeat_timer, 0, sizeof(repeat_timer));
}

static uint32_t map_menu_buttons(void) {
    uint32_t b = pad.buttons;
    uint32_t out = 0;
    if ((b & SCE_CTRL_UP) || pad.ly < 128 - STICK_DEAD) out |= BTN_UP;
    if ((b & SCE_CTRL_DOWN) || pad.ly > 128 + STICK_DEAD) out |= BTN_DOWN;
    if ((b & SCE_CTRL_LEFT) || pad.lx < 128 - STICK_DEAD) out |= BTN_LEFT;
    if ((b & SCE_CTRL_RIGHT) || pad.lx > 128 + STICK_DEAD) out |= BTN_RIGHT;
    uint32_t confirm = confirm_circle ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS;
    uint32_t back = confirm_circle ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE;
    if (b & confirm) out |= BTN_CONFIRM;
    if (b & back) out |= BTN_BACK;
    if (b & SCE_CTRL_TRIANGLE) out |= BTN_TRIANGLE;
    if (b & SCE_CTRL_SQUARE) out |= BTN_SQUARE;
    if (b & (SCE_CTRL_LTRIGGER | SCE_CTRL_L1)) out |= BTN_L;
    if (b & (SCE_CTRL_RTRIGGER | SCE_CTRL_R1)) out |= BTN_R;
    if (b & SCE_CTRL_START) out |= BTN_START;
    if (b & SCE_CTRL_SELECT) out |= BTN_SELECT;
    return out;
}

void input_poll(InputFrame *in) {
    memset(&pad, 0, sizeof(pad));
    pad.lx = pad.ly = pad.rx = pad.ry = 128;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    memset(&touch_front, 0, sizeof(touch_front));
    memset(&touch_back, 0, sizeof(touch_back));
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch_front, 1);
    sceTouchPeek(SCE_TOUCH_PORT_BACK, &touch_back, 1);

    bool ps_now = (pad.buttons & SCE_CTRL_PSBUTTON) != 0;
    ps_released = ps_locked && ps_prev && !ps_now;
    ps_prev = ps_now;
    if (wait_all_released && (pad.buttons & ~SCE_CTRL_PSBUTTON) == 0 && !ps_now && pad.lx > 128 - STICK_DEAD && pad.lx < 128 + STICK_DEAD && pad.ly > 128 - STICK_DEAD && pad.ly < 128 + STICK_DEAD && pad.rx > 128 - STICK_DEAD && pad.rx < 128 + STICK_DEAD && pad.ry > 128 - STICK_DEAD && pad.ry < 128 + STICK_DEAD)
        wait_all_released = false;

    uint32_t held = map_menu_buttons();
    in->held = held;
    in->pressed = held & ~prev_held;
    in->repeat = in->pressed;
    static const uint32_t dirs[4] = {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT};
    for (int i = 0; i < 4; i++) {
        if (held & dirs[i]) {
            repeat_timer[i]++;
            if (repeat_timer[i] > REPEAT_DELAY && ((repeat_timer[i] - REPEAT_DELAY) % REPEAT_RATE) == 0)
                in->repeat |= dirs[i];
        } else {
            repeat_timer[i] = 0;
        }
    }
    prev_held = held;

    bool front = touch_front.reportNum > 0;
    in->touch_front = front;
    in->touch_front_tap = front && !prev_front_touch;
    if (front) {
        in->touch_x = touch_front.report[0].x / 2;
        in->touch_y = touch_front.report[0].y / 2;
    } else {
        in->touch_x = in->touch_y = -1;
    }
    prev_front_touch = front;
    in->lx = pad.lx;
    in->ly = pad.ly;
    in->rx = pad.rx;
    in->ry = pad.ry;
}

static uint16_t sanitize(uint16_t k) {
    if ((k & VB_LPAD_L) && (k & VB_LPAD_R)) k &= ~(VB_LPAD_L | VB_LPAD_R);
    if ((k & VB_LPAD_U) && (k & VB_LPAD_D)) k &= ~(VB_LPAD_U | VB_LPAD_D);
    if ((k & VB_RPAD_L) && (k & VB_RPAD_R)) k &= ~(VB_RPAD_L | VB_RPAD_R);
    if ((k & VB_RPAD_U) && (k & VB_RPAD_D)) k &= ~(VB_RPAD_U | VB_RPAD_D);
    return k;
}

uint16_t input_vb_keys(const InputFrame *in) {
    (void)in;
    uint32_t b = pad.buttons;
    uint16_t k = 0;
    if ((b & SCE_CTRL_UP) || pad.ly < 128 - STICK_DEAD) k |= VB_LPAD_U;
    if ((b & SCE_CTRL_DOWN) || pad.ly > 128 + STICK_DEAD) k |= VB_LPAD_D;
    if ((b & SCE_CTRL_LEFT) || pad.lx < 128 - STICK_DEAD) k |= VB_LPAD_L;
    if ((b & SCE_CTRL_RIGHT) || pad.lx > 128 + STICK_DEAD) k |= VB_LPAD_R;
    if (b & (SCE_CTRL_LTRIGGER | SCE_CTRL_L1)) k |= VB_KEY_L;
    if (b & (SCE_CTRL_RTRIGGER | SCE_CTRL_R1)) k |= VB_KEY_R;
    if (b & SCE_CTRL_START) k |= VB_KEY_START;
    if (b & SCE_CTRL_SELECT) k |= VB_KEY_SELECT;
    uint16_t key_a = settings.swap_ab ? VB_KEY_B : VB_KEY_A;
    uint16_t key_b = settings.swap_ab ? VB_KEY_A : VB_KEY_B;
    if (settings.controls == CONTROLS_DUAL_PAD) {
        if (b & SCE_CTRL_TRIANGLE) k |= VB_RPAD_U;
        if (b & SCE_CTRL_CROSS) k |= VB_RPAD_D;
        if (b & SCE_CTRL_SQUARE) k |= VB_RPAD_L;
        if (b & SCE_CTRL_CIRCLE) k |= VB_RPAD_R;
        if (pad.rx > 128 + STICK_DEAD) k |= key_a;
        if (pad.rx < 128 - STICK_DEAD) k |= key_b;
        for (int i = 0; i < touch_back.reportNum; i++) {
            if (touch_back.report[i].x >= 960) k |= key_a;
            else k |= key_b;
        }
    } else {
        if (pad.ry < 128 - STICK_DEAD) k |= VB_RPAD_U;
        if (pad.ry > 128 + STICK_DEAD) k |= VB_RPAD_D;
        if (pad.rx < 128 - STICK_DEAD) k |= VB_RPAD_L;
        if (pad.rx > 128 + STICK_DEAD) k |= VB_RPAD_R;
        if (b & SCE_CTRL_CIRCLE) k |= key_a;
        if (b & (SCE_CTRL_CROSS | SCE_CTRL_SQUARE)) k |= key_b;
    }
    return sanitize(k);
}

bool input_menu_combo(const InputFrame *in) {
    return in->touch_front_tap || ps_released;
}

void input_capture_ps_button(bool capture) {
    if (capture) {
        if (!ps_locked) sceShellUtilLock(PS_LOCK_FLAGS);
        ps_locked = true;
        ps_prev = false;
        wait_all_released = true;
    } else {
        if (ps_locked) sceShellUtilUnlock(PS_LOCK_FLAGS);
        ps_locked = false;
        ps_prev = false;
    }
}

void input_release_ps_button_now(void) {
    input_capture_ps_button(false);
}

bool input_game_blocked(void) {
    return wait_all_released;
}

bool input_fast_forward(const InputFrame *in) {
    if (settings.controls == CONTROLS_DUAL_PAD) return ff_latched;
    if (settings.ff_toggle) {
        if (in->pressed & BTN_TRIANGLE) ff_latched = !ff_latched;
        return ff_latched;
    }
    return (in->held & BTN_TRIANGLE) != 0;
}

void input_set_fast_forward_latch(bool on) {
    ff_latched = on;
}

bool input_get_fast_forward_latch(void) {
    return ff_latched;
}
