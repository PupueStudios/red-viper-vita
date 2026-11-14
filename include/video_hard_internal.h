#ifndef VIDEO_HARD_INTERNAL_H
#define VIDEO_HARD_INTERNAL_H

#include "vb_types.h"
#include "vb_dsp.h"
#include "video_hard.h"

#include <vitaGL.h>
typedef GLuint Texture, Framebuffer;

extern Framebuffer screenTargetHard[2];

// Virtual Bowling needs at least 4 for good performance
#define AFFINE_CACHE_SIZE 8
typedef struct {
	Texture tex;
	Framebuffer target;
	int bg;
	short umin, umax, vmin, vmax;
	short lumin, lumax, lvmin, lvmax;
	u16 tiles[64 * 64];
	u8 GPLT[4];
	bool visible;
	bool used;
} AffineCacheEntry;
extern AffineCacheEntry tileMapCache[AFFINE_CACHE_SIZE];

typedef struct {
	short x, y;
	u8 u, v, palette, orient;
} __attribute__((aligned(4))) vertex;

typedef struct {
	short x1, y1, mx1, my1, dx1, dy1, mp1, off1;
	short x2, y2, mx2, my2, dx2, dy2, mp2, off2;
} avertex;

#define VBUF_SIZE 64 * 64 * 2 * 32
extern vertex *vbuf, *vcur;
#define AVBUF_SIZE 4096 * 8
extern avertex *avbuf, *avcur;

// 224 works on hardware but breaks in azahar
#define DOWNLOADED_FRAMEBUFFER_WIDTH 256
extern uint16_t *rgba4_framebuffers;

void gpu_setup_screen(int start_eye, int end_eye);
void gpu_setup_drawing(void);
void gpu_setup_tile_drawing(void);
void gpu_set_tile_offset(float xoffset, float yoffset);
void gpu_init_vip_download(int start_eye, int end_eye, int drawn_fb);
void gpu_target_screen(int drawn_fb);
void gpu_target_affine(int cache_id);
void gpu_set_scissor(bool enabled, u32 left, u32 top, u32 right, u32 bottom);
void gpu_draw_tiles(int first, int count);
bool gpu_can_chunk(void);
void gpu_draw_affine(WORLD *world, int umin, int vmin, int umax, int vmax, int drawn_fb, bool visible[]);

#endif
