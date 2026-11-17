#ifndef RV_VITA_COLORPACK_H
#define RV_VITA_COLORPACK_H

#include <stdbool.h>
#include <stdint.h>

#define CP_BASE_SLOTS 2048
#define CP_TOTAL_SLOTS 4096

typedef struct {
    float world_ramp[3][3];
    float sprite_ramps[4][3][3];
    float base[4][3];
    bool figure;
} CpShading;

extern bool cp_frame_on;

bool cp_load_for_game(const char *rom_path, const char *game_name, uint32_t crc, uint32_t size);
void cp_unload(void);
bool cp_has_pack(void);
bool cp_display_on(void);
void cp_invalidate(void);

void cp_frame_begin(const void *worlds, bool tocpu);
void cp_world_begin(int wrld);
bool cp_world_used_fill(void);
const CpShading *cp_shading(void);

int cp_bg_slot(int chr, int palette, int cell, int sx, int sy);
int cp_obj_slot(int chr, int palette, int sx, int sy);
int cp_affine_slot(int chr, int palette, int cell);
void cp_mark_obj(int chr, int sx, int sy);
void cp_mark_bg(int chr, int sx, int sy);

void cp_slot_changed(int chr, const uint16_t rows[8]);

int cp_brightness_level(void);
float cp_fade(void);
bool cp_shade_off(int shade);

void gpu_slot_colors(int slot, const uint32_t colors[64]);
void gpu_slot_variant(int slot, int src_chr, const uint32_t colors[64]);
void gpu_reset_color_state(void);

#endif
