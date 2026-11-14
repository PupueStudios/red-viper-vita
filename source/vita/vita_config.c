#include <psp2/power.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inih/ini.h"
#include "vb_set.h"
#include "vita.h"
#include "vita_color.h"

VitaSettings settings;

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

void settings_defaults(void) {
    memset(&settings, 0, sizeof(settings));
    settings.scale_mode = SCALE_INTEGER;
    settings.smooth = 0;
    settings.stereo = STEREO_LEFT;
    settings.anaglyph = ANAGLYPH_RED_CYAN;
    settings.color_mode = COLOR_TINT;
    settings.tint = 0;
    settings.multicolor = 0;
    settings.gradient = 0;
    settings.show_fps = 0;
    settings.sound = 1;
    settings.volume = 10;
    settings.controls = CONTROLS_STANDARD;
    settings.swap_ab = 0;
    settings.ff_toggle = 0;
    settings.overclock = 1;
    settings.led_grid = 0;
    strcpy(settings.last_dir, RV_ROM_DIR);
}

static int vita_ini_handler(void *user, const char *section, const char *name, const char *value) {
    VitaSettings *s = user;
    (void)section;
    int v = atoi(value);
    if (!strcmp(name, "scale")) s->scale_mode = clampi(v, 0, SCALE_COUNT - 1);
    else if (!strcmp(name, "smooth")) s->smooth = !!v;
    else if (!strcmp(name, "stereo")) s->stereo = clampi(v, 0, STEREO_COUNT - 1);
    else if (!strcmp(name, "anaglyph")) s->anaglyph = clampi(v, 0, ANAGLYPH_COUNT - 1);
    else if (!strcmp(name, "color_mode")) s->color_mode = clampi(v, 0, COLOR_MODE_COUNT - 1);
    else if (!strcmp(name, "tint")) s->tint = clampi(v, 0, COLOR_TINT_COUNT - 1);
    else if (!strcmp(name, "multicolor")) s->multicolor = clampi(v, 0, COLOR_MULTICOLOR_COUNT - 1);
    else if (!strcmp(name, "gradient")) s->gradient = clampi(v, 0, COLOR_GRADIENT_COUNT - 1);
    else if (!strcmp(name, "show_fps")) s->show_fps = !!v;
    else if (!strcmp(name, "sound")) s->sound = !!v;
    else if (!strcmp(name, "volume")) s->volume = clampi(v, 0, 10);
    else if (!strcmp(name, "controls")) s->controls = clampi(v, 0, CONTROLS_COUNT - 1);
    else if (!strcmp(name, "swap_ab")) s->swap_ab = !!v;
    else if (!strcmp(name, "ff_toggle")) s->ff_toggle = !!v;
    else if (!strcmp(name, "overclock")) s->overclock = !!v;
    else if (!strcmp(name, "led_grid")) s->led_grid = !!v;
    else if (!strcmp(name, "last_dir")) {
        strncpy(s->last_dir, value, sizeof(s->last_dir) - 1);
        s->last_dir[sizeof(s->last_dir) - 1] = 0;
    }
    return 1;
}

void settings_load(void) {
    settings_defaults();
    ini_parse(RV_CONFIG_PATH, vita_ini_handler, &settings);
}

void settings_save(void) {
    FILE *f = fopen(RV_CONFIG_PATH, "w");
    if (!f) return;
    fprintf(f, "[vita]\n");
    fprintf(f, "scale=%d\n", settings.scale_mode);
    fprintf(f, "smooth=%d\n", settings.smooth);
    fprintf(f, "stereo=%d\n", settings.stereo);
    fprintf(f, "anaglyph=%d\n", settings.anaglyph);
    int choice[4];
    color_global_choice(choice);
    fprintf(f, "color_mode=%d\n", choice[0]);
    fprintf(f, "tint=%d\n", choice[1]);
    fprintf(f, "multicolor=%d\n", choice[2]);
    fprintf(f, "gradient=%d\n", choice[3]);
    fprintf(f, "show_fps=%d\n", settings.show_fps);
    fprintf(f, "sound=%d\n", settings.sound);
    fprintf(f, "volume=%d\n", settings.volume);
    fprintf(f, "controls=%d\n", settings.controls);
    fprintf(f, "swap_ab=%d\n", settings.swap_ab);
    fprintf(f, "ff_toggle=%d\n", settings.ff_toggle);
    fprintf(f, "overclock=%d\n", settings.overclock);
    fprintf(f, "led_grid=%d\n", settings.led_grid);
    fprintf(f, "last_dir=%s\n", settings.last_dir);
    fclose(f);
}

void settings_apply(void) {
    tVBOpt.ANAGLYPH = settings.stereo == STEREO_ANAGLYPH;
    tVBOpt.MULTICOL = false;
    tVBOpt.SOUND = settings.sound;
    tVBOpt.DEFAULT_EYE = settings.stereo == STEREO_RIGHT ? 1 : 0;
    if (settings.overclock) {
        scePowerSetArmClockFrequency(444);
        scePowerSetBusClockFrequency(222);
        scePowerSetGpuClockFrequency(222);
        scePowerSetGpuXbarClockFrequency(166);
    } else {
        scePowerSetArmClockFrequency(333);
        scePowerSetBusClockFrequency(222);
        scePowerSetGpuClockFrequency(166);
        scePowerSetGpuXbarClockFrequency(111);
    }
    video_apply_filter();
}
