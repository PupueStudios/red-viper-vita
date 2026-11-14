#include <malloc.h>
#include <math.h>
#include <png.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/pgf.h>
#include <psp2/power.h>
#include <psp2/rtc.h>
#include <psp2/sysmodule.h>
#include <stdio.h>
#include <jpeglib.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "vita.h"
#include "vita_ui.h"

#define CORNER_SEGS 8
#define FONT_ASCENT 17.0f
#define BATCH_MAX 6144
#define SCRATCH_MAX 512
#define ATLAS_W 512
#define ATLAS_H 512
#define GLYPH_SLOTS 2048

enum {
    PRIM_TRIS,
    PRIM_STRIP,
    PRIM_FAN,
};

enum {
    TEXMODE_SOLID,
    TEXMODE_RGBA,
    TEXMODE_ALPHA,
};

typedef struct {
    unsigned int code;
    short x, y, w, h;
    short left, top;
    short advance;
    bool used;
    bool valid;
} Glyph;

static GLuint ui_program = 0;
static GLint loc_scale, loc_mode, loc_tex;
static GLuint white_tex = 0;
static UiVert batch[BATCH_MAX];
static int batch_count = 0;
static GLuint batch_tex = 0;
static int batch_mode = TEXMODE_SOLID;
static UiVert scratch[SCRATCH_MAX];

static int clip_depth = 0;
static int clip_stack[8][4];

static SceFontLibHandle font_lib = NULL;
static SceFontHandle font_handle = NULL;
static uint8_t *atlas_pixels = NULL;
static GLuint atlas_tex = 0;
static int atlas_x = 0, atlas_y = 0, atlas_row = 0;
static Glyph glyphs[GLYPH_SLOTS];

static UiTexture *logo_tex = NULL;
static uint64_t start_us = 0;
static uint64_t last_us = 0;
static float now_s = 0;
static float delta_s = 0;
static char toast_msg[128];
static float toast_until = -1;
static float toast_born = 0;

GLuint vita_build_program(const char *vs, const char *fs, const char *const *attribs, int attrib_count);

static const char *const ui_attribs[] = {"aPos", "aUV", "aColor"};

static void *pgf_alloc(void *userdata, unsigned int size) {
    (void)userdata;
    return memalign(sizeof(int), (size + sizeof(int) - 1) / sizeof(int) * sizeof(int));
}

static void pgf_free(void *userdata, void *p) {
    (void)userdata;
    free(p);
}

static void font_init(void) {
    if (sceSysmoduleIsLoaded(SCE_SYSMODULE_PGF) != SCE_SYSMODULE_LOADED) sceSysmoduleLoadModule(SCE_SYSMODULE_PGF);
    unsigned int error = 0;
    SceFontNewLibParams params = {NULL, 1, NULL, pgf_alloc, pgf_free, NULL, NULL, NULL, NULL, NULL, NULL};
    font_lib = sceFontNewLib(&params, &error);
    if (error != 0) {
        font_lib = NULL;
        return;
    }
    SceFontStyle style;
    memset(&style, 0, sizeof(style));
    style.fontH = 10;
    style.fontV = 10;
    style.fontLanguage = SCE_FONT_LANGUAGE_DEFAULT;
    int index = sceFontFindOptimumFont(font_lib, &style, &error);
    if (error != 0) return;
    font_handle = sceFontOpen(font_lib, index, 0, &error);
    if (error != 0) font_handle = NULL;
    atlas_pixels = calloc(ATLAS_W * ATLAS_H, 1);
    glGenTextures(1, &atlas_tex);
    glBindTexture(GL_TEXTURE_2D, atlas_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, ATLAS_W, ATLAS_H, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, atlas_pixels);
}

static void atlas_reset(void) {
    ui_flush();
    memset(glyphs, 0, sizeof(glyphs));
    memset(atlas_pixels, 0, ATLAS_W * ATLAS_H);
    atlas_x = atlas_y = atlas_row = 0;
    glBindTexture(GL_TEXTURE_2D, atlas_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, ATLAS_W, ATLAS_H, GL_LUMINANCE, GL_UNSIGNED_BYTE, atlas_pixels);
}

static Glyph *glyph_get(unsigned int code) {
    if (!font_handle) return NULL;
    unsigned int slot = (code * 2654435761u) % GLYPH_SLOTS;
    for (int probe = 0; probe < GLYPH_SLOTS; probe++) {
        Glyph *g = &glyphs[(slot + probe) % GLYPH_SLOTS];
        if (g->used && g->code == code) return g->valid ? g : NULL;
        if (!g->used) {
            SceFontCharInfo info;
            g->used = true;
            g->code = code;
            g->valid = false;
            if (sceFontGetCharInfo(font_handle, code, &info) < 0) return NULL;
            int w = info.bitmapWidth, h = info.bitmapHeight;
            if (atlas_x + w + 1 > ATLAS_W) {
                atlas_x = 0;
                atlas_y += atlas_row + 1;
                atlas_row = 0;
            }
            if (atlas_y + h + 1 > ATLAS_H) {
                atlas_reset();
                return glyph_get(code);
            }
            g->x = atlas_x;
            g->y = atlas_y;
            g->w = w;
            g->h = h;
            g->left = info.bitmapLeft;
            g->top = info.bitmapTop;
            g->advance = info.sfp26AdvanceH >> 6;
            if (w > 0 && h > 0) {
                SceFontGlyphImage img;
                memset(&img, 0, sizeof(img));
                img.pixelFormat = SCE_FONT_PIXELFORMAT_8;
                img.xPos64 = atlas_x << 6;
                img.yPos64 = atlas_y << 6;
                img.bufWidth = ATLAS_W;
                img.bufHeight = ATLAS_H;
                img.bytesPerLine = ATLAS_W;
                img.bufferPtr = (unsigned int)atlas_pixels;
                sceFontGetCharGlyphImage(font_handle, code, &img);
                ui_flush();
                glBindTexture(GL_TEXTURE_2D, atlas_tex);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, atlas_y, ATLAS_W, h, GL_LUMINANCE, GL_UNSIGNED_BYTE, atlas_pixels + atlas_y * ATLAS_W);
            }
            atlas_x += w + 1;
            if (h > atlas_row) atlas_row = h;
            g->valid = true;
            return g;
        }
    }
    return NULL;
}

static int utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *s += 3;
        return ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *s += 2;
        return ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    }
    *s += 1;
    return p[0];
}

static void apply_clip(void) {
    if (clip_depth == 0) {
        glDisable(GL_SCISSOR_TEST);
        return;
    }
    int *r = clip_stack[clip_depth - 1];
    int w = r[2] - r[0], h = r[3] - r[1];
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    glEnable(GL_SCISSOR_TEST);
    glScissor(r[0], SCREEN_H - r[3], w, h);
}

void ui_restore_state(void) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, SCREEN_W, SCREEN_H);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(ui_program);
    glUniform2f(loc_scale, 2.0f / SCREEN_W, 2.0f / SCREEN_H);
    glUniform1i(loc_tex, 0);
    apply_clip();
}

void ui_flush(void) {
    if (batch_count == 0) return;
    glUseProgram(ui_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, batch_tex ? batch_tex : white_tex);
    glUniform1f(loc_mode, (float)batch_mode);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(UiVert), &batch[0].x);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(UiVert), &batch[0].u);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(UiVert), &batch[0].c);
    glDrawArrays(GL_TRIANGLES, 0, batch_count);
    batch_count = 0;
}

static void batch_begin(GLuint tex, int mode, int verts_needed) {
    if (batch_count > 0 && (tex != batch_tex || mode != batch_mode || batch_count + verts_needed > BATCH_MAX)) ui_flush();
    batch_tex = tex;
    batch_mode = mode;
}

static void push_tri(const UiVert *a, const UiVert *b, const UiVert *c) {
    batch[batch_count++] = *a;
    batch[batch_count++] = *b;
    batch[batch_count++] = *c;
}

static void submit(int prim, const UiVert *v, int n, GLuint tex, int mode) {
    if (n < 3) return;
    int tris = prim == PRIM_TRIS ? n / 3 : n - 2;
    if (tris * 3 > BATCH_MAX) return;
    batch_begin(tex, mode, tris * 3);
    if (prim == PRIM_TRIS) {
        for (int i = 0; i + 2 < n; i += 3) push_tri(&v[i], &v[i + 1], &v[i + 2]);
    } else if (prim == PRIM_STRIP) {
        for (int i = 2; i < n; i++) push_tri(&v[i - 2], &v[i - 1], &v[i]);
    } else {
        for (int i = 2; i < n; i++) push_tri(&v[0], &v[i - 1], &v[i]);
    }
}

static UiVert *verts(int n) {
    return n <= SCRATCH_MAX ? scratch : NULL;
}

static void set_v(UiVert *v, float x, float y, uint32_t c) {
    v->x = x;
    v->y = y;
    v->u = 0;
    v->v = 0;
    v->c = c;
}

static void quad(float x, float y, float w, float h, float u0, float v0, float u1, float v1, uint32_t c, GLuint tex, int mode) {
    UiVert q[4] = {
        {x, y, u0, v0, c},
        {x + w, y, u1, v0, c},
        {x, y + h, u0, v1, c},
        {x + w, y + h, u1, v1, c},
    };
    submit(PRIM_STRIP, q, 4, tex, mode);
}

void ui_clip_push(float x0, float y0, float x1, float y1) {
    ui_flush();
    int r[4] = {(int)x0, (int)y0, (int)x1, (int)y1};
    if (clip_depth > 0) {
        int *p = clip_stack[clip_depth - 1];
        if (r[0] < p[0]) r[0] = p[0];
        if (r[1] < p[1]) r[1] = p[1];
        if (r[2] > p[2]) r[2] = p[2];
        if (r[3] > p[3]) r[3] = p[3];
    }
    if (clip_depth < 8) memcpy(clip_stack[clip_depth++], r, sizeof(r));
    apply_clip();
}

void ui_clip_pop(void) {
    ui_flush();
    if (clip_depth > 0) clip_depth--;
    apply_clip();
}

UiTexture *ui_texture_create(int w, int h, const uint32_t *rgba) {
    UiTexture *t = calloc(1, sizeof(UiTexture));
    if (!t) return NULL;
    t->w = w;
    t->h = h;
    glGenTextures(1, &t->id);
    glBindTexture(GL_TEXTURE_2D, t->id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return t;
}

void ui_texture_update(UiTexture *t, const uint32_t *rgba) {
    if (!t) return;
    ui_flush();
    glBindTexture(GL_TEXTURE_2D, t->id);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, t->w, t->h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

void ui_texture_free(UiTexture *t) {
    if (!t) return;
    ui_flush();
    glDeleteTextures(1, &t->id);
    free(t);
}

void ui_texture_filter(UiTexture *t, bool linear) {
    if (!t) return;
    ui_flush();
    glBindTexture(GL_TEXTURE_2D, t->id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
}

static uint32_t *load_png(const char *path, int *w, int *h) {
    png_image img;
    memset(&img, 0, sizeof(img));
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&img, path)) return NULL;
    img.format = PNG_FORMAT_RGBA;
    uint32_t *px = malloc(PNG_IMAGE_SIZE(img));
    if (!px) {
        png_image_free(&img);
        return NULL;
    }
    if (!png_image_finish_read(&img, NULL, px, 0, NULL)) {
        free(px);
        return NULL;
    }
    *w = img.width;
    *h = img.height;
    return px;
}

static uint32_t *load_jpeg(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, f);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);
    int ww = cinfo.output_width, hh = cinfo.output_height;
    uint32_t *px = malloc((size_t)ww * hh * 4);
    uint8_t *row = malloc((size_t)ww * 3);
    if (px && row) {
        while (cinfo.output_scanline < cinfo.output_height) {
            int y = cinfo.output_scanline;
            JSAMPROW rows[1] = {row};
            jpeg_read_scanlines(&cinfo, rows, 1);
            for (int x = 0; x < ww; x++)
                px[y * ww + x] = 0xFF000000u | (row[x * 3 + 2] << 16) | (row[x * 3 + 1] << 8) | row[x * 3];
        }
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(f);
    free(row);
    if (!px) return NULL;
    *w = ww;
    *h = hh;
    return px;
}

UiTexture *ui_texture_load(const char *path) {
    const char *dot = strrchr(path, '.');
    int w = 0, h = 0;
    uint32_t *px = NULL;
    if (dot && (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg"))) px = load_jpeg(path, &w, &h);
    else px = load_png(path, &w, &h);
    if (!px) return NULL;
    UiTexture *t = ui_texture_create(w, h, px);
    free(px);
    return t;
}

void ui_draw_texture(const UiTexture *t, float x, float y, float w, float h, uint32_t tint) {
    if (!t) return;
    quad(x, y, w, h, 0, 0, 1, 1, tint, t->id, TEXMODE_RGBA);
}

void ui_draw_gl_texture(GLuint id, float x, float y, float w, float h, float u0, float v0, float u1, float v1, uint32_t tint) {
    quad(x, y, w, h, u0, v0, u1, v1, tint, id, TEXMODE_RGBA);
}

void ui_init(void) {
    ui_program = vita_build_program(
        "attribute vec2 aPos;\n"
        "attribute vec2 aUV;\n"
        "attribute vec4 aColor;\n"
        "uniform vec2 uScale;\n"
        "varying vec2 vUV;\n"
        "varying vec4 vColor;\n"
        "void main() {\n"
        "   gl_Position = vec4(aPos.x * uScale.x - 1.0, 1.0 - aPos.y * uScale.y, 0.0, 1.0);\n"
        "   vUV = aUV;\n"
        "   vColor = aColor;\n"
        "}\n",
        "uniform sampler2D sTex;\n"
        "uniform float uMode;\n"
        "varying vec2 vUV;\n"
        "varying vec4 vColor;\n"
        "void main() {\n"
        "   vec4 t = texture2D(sTex, vUV);\n"
        "   if (uMode > 1.5) gl_FragColor = vec4(vColor.rgb, vColor.a * t.r);\n"
        "   else if (uMode > 0.5) gl_FragColor = vColor * t;\n"
        "   else gl_FragColor = vColor;\n"
        "}\n",
        ui_attribs, 3);
    loc_scale = glGetUniformLocation(ui_program, "uScale");
    loc_mode = glGetUniformLocation(ui_program, "uMode");
    loc_tex = glGetUniformLocation(ui_program, "sTex");
    uint32_t white = 0xFFFFFFFF;
    glGenTextures(1, &white_tex);
    glBindTexture(GL_TEXTURE_2D, white_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
    font_init();
    logo_tex = ui_texture_load("app0:assets/logo.png");
    start_us = last_us = sceKernelGetProcessTimeWide();
}

void ui_shutdown(void) {
    ui_texture_free(logo_tex);
    logo_tex = NULL;
    if (font_handle) sceFontClose(font_handle);
    if (font_lib) sceFontDoneLib(font_lib);
    font_handle = NULL;
    font_lib = NULL;
    free(atlas_pixels);
    atlas_pixels = NULL;
}

void ui_frame_begin(void) {
    uint64_t t = sceKernelGetProcessTimeWide();
    delta_s = (t - last_us) / 1000000.0f;
    if (delta_s > 0.1f) delta_s = 0.1f;
    last_us = t;
    now_s = (t - start_us) / 1000000.0f;
    clip_depth = 0;
    batch_count = 0;
    ui_restore_state();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

void ui_frame_end(void) {
    ui_flush();
    vglSwapBuffers(GL_FALSE);
}

float ui_time(void) {
    return now_s;
}

float ui_dt(void) {
    return delta_s;
}

uint32_t ui_alpha(uint32_t color, float a) {
    if (a < 0) a = 0;
    if (a > 1) a = 1;
    uint32_t al = (uint32_t)(((color >> 24) & 0xFF) * a);
    return (color & 0x00FFFFFF) | (al << 24);
}

uint32_t ui_mix(uint32_t a, uint32_t b, float t) {
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    uint32_t out = 0;
    for (int i = 0; i < 4; i++) {
        float ca = (a >> (i * 8)) & 0xFF;
        float cb = (b >> (i * 8)) & 0xFF;
        uint32_t c = (uint32_t)(ca + (cb - ca) * t + 0.5f);
        out |= (c & 0xFF) << (i * 8);
    }
    return out;
}

float ui_approach(float current, float target, float speed) {
    float k = 1.0f - expf(-speed * delta_s);
    return current + (target - current) * k;
}

float ui_ease_out(float t) {
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}

void ui_rect(float x, float y, float w, float h, uint32_t c) {
    if (w <= 0 || h <= 0) return;
    quad(x, y, w, h, 0, 0, 0, 0, c, 0, TEXMODE_SOLID);
}

void ui_gradient_v(float x, float y, float w, float h, uint32_t top, uint32_t bottom) {
    UiVert v[4];
    set_v(&v[0], x, y, top);
    set_v(&v[1], x + w, y, top);
    set_v(&v[2], x, y + h, bottom);
    set_v(&v[3], x + w, y + h, bottom);
    submit(PRIM_STRIP, v, 4, 0, TEXMODE_SOLID);
}

void ui_gradient_h(float x, float y, float w, float h, uint32_t left, uint32_t right) {
    UiVert v[4];
    set_v(&v[0], x, y, left);
    set_v(&v[1], x + w, y, right);
    set_v(&v[2], x, y + h, left);
    set_v(&v[3], x + w, y + h, right);
    submit(PRIM_STRIP, v, 4, 0, TEXMODE_SOLID);
}

static int perimeter(float *px, float *py, float x, float y, float w, float h, float r) {
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 0) r = 0;
    float cx[4] = {x + w - r, x + r, x + r, x + w - r};
    float cy[4] = {y + r, y + r, y + h - r, y + h - r};
    int n = 0;
    for (int c = 0; c < 4; c++) {
        float a0 = -M_PI / 2 * c;
        for (int i = 0; i <= CORNER_SEGS; i++) {
            float a = a0 - (M_PI / 2) * i / CORNER_SEGS;
            px[n] = cx[c] + cosf(a) * r;
            py[n] = cy[c] + sinf(a) * r;
            n++;
        }
    }
    return n;
}

void ui_round_rect(float x, float y, float w, float h, float r, uint32_t c) {
    if (w <= 0 || h <= 0) return;
    float px[4 * (CORNER_SEGS + 1)], py[4 * (CORNER_SEGS + 1)];
    int n = perimeter(px, py, x, y, w, h, r);
    UiVert *v = verts(n + 2);
    set_v(&v[0], x + w / 2, y + h / 2, c);
    for (int i = 0; i < n; i++) set_v(&v[i + 1], px[i], py[i], c);
    set_v(&v[n + 1], px[0], py[0], c);
    submit(PRIM_FAN, v, n + 2, 0, TEXMODE_SOLID);
}

void ui_round_outline(float x, float y, float w, float h, float r, uint32_t c) {
    float ox[4 * (CORNER_SEGS + 1)], oy[4 * (CORNER_SEGS + 1)];
    float ix[4 * (CORNER_SEGS + 1)], iy[4 * (CORNER_SEGS + 1)];
    int n = perimeter(ox, oy, x, y, w, h, r);
    perimeter(ix, iy, x + 1.5f, y + 1.5f, w - 3, h - 3, r - 1.5f > 0 ? r - 1.5f : 0);
    UiVert *v = verts(n * 2 + 2);
    for (int i = 0; i < n; i++) {
        set_v(&v[i * 2], ox[i], oy[i], c);
        set_v(&v[i * 2 + 1], ix[i], iy[i], c);
    }
    set_v(&v[n * 2], ox[0], oy[0], c);
    set_v(&v[n * 2 + 1], ix[0], iy[0], c);
    submit(PRIM_STRIP, v, n * 2 + 2, 0, TEXMODE_SOLID);
}

void ui_thick_line(float x0, float y0, float x1, float y1, float t, uint32_t c) {
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) return;
    float nx = -dy / len * t / 2, ny = dx / len * t / 2;
    UiVert v[4];
    set_v(&v[0], x0 + nx, y0 + ny, c);
    set_v(&v[1], x1 + nx, y1 + ny, c);
    set_v(&v[2], x0 - nx, y0 - ny, c);
    set_v(&v[3], x1 - nx, y1 - ny, c);
    submit(PRIM_STRIP, v, 4, 0, TEXMODE_SOLID);
}

void ui_line(float x0, float y0, float x1, float y1, uint32_t c) {
    ui_thick_line(x0, y0, x1, y1, 1.0f, c);
}

void ui_circle(float x, float y, float r, uint32_t c) {
    int segs = r < 8 ? 16 : 32;
    UiVert *v = verts(segs + 2);
    set_v(&v[0], x, y, c);
    for (int i = 0; i <= segs; i++) {
        float a = 2 * M_PI * i / segs;
        set_v(&v[i + 1], x + cosf(a) * r, y + sinf(a) * r, c);
    }
    submit(PRIM_FAN, v, segs + 2, 0, TEXMODE_SOLID);
}

void ui_ring(float x, float y, float r, float t, uint32_t c) {
    int segs = 32;
    UiVert *v = verts((segs + 1) * 2);
    for (int i = 0; i <= segs; i++) {
        float a = 2 * M_PI * i / segs;
        set_v(&v[i * 2], x + cosf(a) * r, y + sinf(a) * r, c);
        set_v(&v[i * 2 + 1], x + cosf(a) * (r - t), y + sinf(a) * (r - t), c);
    }
    submit(PRIM_STRIP, v, (segs + 1) * 2, 0, TEXMODE_SOLID);
}

static int text_run(float x, float y, uint32_t c, float scale, const char *s, bool draw) {
    float pen = x;
    float baseline = y + FONT_ASCENT * scale;
    while (s && *s) {
        unsigned int code = utf8_next(&s);
        Glyph *g = glyph_get(code);
        if (!g) continue;
        if (draw && g->w > 0 && g->h > 0) {
            float u0 = (float)g->x / ATLAS_W, v0 = (float)g->y / ATLAS_H;
            float u1 = (float)(g->x + g->w) / ATLAS_W, v1 = (float)(g->y + g->h) / ATLAS_H;
            quad(pen + g->left * scale, baseline - g->top * scale, g->w * scale, g->h * scale, u0, v0, u1, v1, c, atlas_tex, TEXMODE_ALPHA);
        }
        pen += g->advance * scale;
    }
    return (int)(pen - x);
}

int ui_text(float x, float y, uint32_t c, float scale, const char *s) {
    return text_run((int)x, (int)y, c, scale, s, true);
}

int ui_text_width(float scale, const char *s) {
    if (!s || !*s) return 0;
    return text_run(0, 0, 0, scale, s, false);
}

int ui_text_right(float x, float y, uint32_t c, float scale, const char *s) {
    return ui_text(x - ui_text_width(scale, s), y, c, scale, s);
}

int ui_text_center(float cx, float y, uint32_t c, float scale, const char *s) {
    return ui_text(cx - ui_text_width(scale, s) / 2.0f, y, c, scale, s);
}

void ui_text_clipped(float x, float y, float max_w, uint32_t c, float scale, const char *s, float scroll_t) {
    int w = ui_text_width(scale, s);
    if (w <= max_w) {
        ui_text(x, y, c, scale, s);
        return;
    }
    float overflow = w - max_w + 24;
    float period = 2.0f + overflow / 40.0f;
    float t = fmodf(scroll_t, period * 2);
    float phase = t < period ? t / period : 2 - t / period;
    phase = phase < 0.2f ? 0 : phase > 0.8f ? 1 : (phase - 0.2f) / 0.6f;
    float off = overflow * phase;
    ui_clip_push(x, y - 4, x + max_w, y + 40 * scale);
    ui_text(x - off, y, c, scale, s);
    ui_clip_pop();
}

static const uint8_t dot_glyphs[][7] = {
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
    {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},
    {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E},
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F},
    {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C},
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11},
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11},
    {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11},
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D},
    {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E},
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A},
    {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},
    {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04},
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F},
};

static const uint8_t dot_digits[][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
};

static const uint8_t *dot_lookup(char ch) {
    static const uint8_t dot_dot[7] = {0, 0, 0, 0, 0, 0x0C, 0x0C};
    static const uint8_t dot_dash[7] = {0, 0, 0, 0x1F, 0, 0, 0};
    static const uint8_t dot_colon[7] = {0, 0x0C, 0x0C, 0, 0x0C, 0x0C, 0};
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    if (ch >= 'A' && ch <= 'Z') return dot_glyphs[ch - 'A'];
    if (ch >= '0' && ch <= '9') return dot_digits[ch - '0'];
    if (ch == '.') return dot_dot;
    if (ch == '-') return dot_dash;
    if (ch == ':') return dot_colon;
    return NULL;
}

float ui_dot_text_width(float dot, const char *s) {
    int n = strlen(s);
    return n > 0 ? n * 6 * dot - dot : 0;
}

void ui_dot_text(float x, float y, float dot, uint32_t c, const char *s) {
    float size = dot * 0.78f;
    for (; *s; s++, x += 6 * dot) {
        const uint8_t *g = dot_lookup(*s);
        if (!g) continue;
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++)
                if (g[row] & (0x10 >> col))
                    ui_rect(x + col * dot, y + row * dot, size, size, c);
    }
}

static void glyph_disc(float cx, float cy, float r, float alpha) {
    ui_circle(cx, cy, r, ui_alpha(RGBA8(255, 255, 255, 34), alpha));
    ui_circle(cx, cy, r - 1.5f, ui_alpha(RGBA8(16, 6, 9, 235), alpha));
}

static float glyph_pill(float x, float cy, const char *label, float alpha) {
    float w = ui_text_width(0.62f, label) + 16;
    ui_round_rect(x, cy - 10, w, 20, 10, ui_alpha(RGBA8(255, 255, 255, 34), alpha));
    ui_round_rect(x + 1.5f, cy - 8.5f, w - 3, 17, 8.5f, ui_alpha(RGBA8(16, 6, 9, 235), alpha));
    ui_text(x + 8, cy - 9, ui_alpha(COL_TEXT, alpha), 0.62f, label);
    return w;
}

float ui_glyph(int glyph, float x, float cy, float alpha) {
    if (glyph == GLYPH_CONFIRM) glyph = input_confirm_is_circle() ? GLYPH_CIRCLE : GLYPH_CROSS;
    if (glyph == GLYPH_BACK) glyph = input_confirm_is_circle() ? GLYPH_CROSS : GLYPH_CIRCLE;
    float r = 11;
    float cx = x + r;
    switch (glyph) {
        case GLYPH_CROSS: {
            uint32_t c = ui_alpha(RGBA8(140, 170, 255, 255), alpha);
            glyph_disc(cx, cy, r, alpha);
            ui_thick_line(cx - 4.5f, cy - 4.5f, cx + 4.5f, cy + 4.5f, 2.2f, c);
            ui_thick_line(cx - 4.5f, cy + 4.5f, cx + 4.5f, cy - 4.5f, 2.2f, c);
            return r * 2;
        }
        case GLYPH_CIRCLE:
            glyph_disc(cx, cy, r, alpha);
            ui_ring(cx, cy, 5.5f, 2.2f, ui_alpha(RGBA8(255, 110, 120, 255), alpha));
            return r * 2;
        case GLYPH_TRIANGLE: {
            uint32_t c = ui_alpha(RGBA8(70, 220, 170, 255), alpha);
            glyph_disc(cx, cy, r, alpha);
            ui_thick_line(cx, cy - 5.5f, cx - 5.5f, cy + 4, 2.0f, c);
            ui_thick_line(cx - 5.5f, cy + 4, cx + 5.5f, cy + 4, 2.0f, c);
            ui_thick_line(cx + 5.5f, cy + 4, cx, cy - 5.5f, 2.0f, c);
            return r * 2;
        }
        case GLYPH_SQUARE: {
            uint32_t c = ui_alpha(RGBA8(255, 140, 220, 255), alpha);
            glyph_disc(cx, cy, r, alpha);
            ui_thick_line(cx - 4.5f, cy - 4.5f, cx + 4.5f, cy - 4.5f, 2.0f, c);
            ui_thick_line(cx + 4.5f, cy - 4.5f, cx + 4.5f, cy + 4.5f, 2.0f, c);
            ui_thick_line(cx + 4.5f, cy + 4.5f, cx - 4.5f, cy + 4.5f, 2.0f, c);
            ui_thick_line(cx - 4.5f, cy + 4.5f, cx - 4.5f, cy - 4.5f, 2.0f, c);
            return r * 2;
        }
        case GLYPH_START:
            return glyph_pill(x, cy, "START", alpha);
        case GLYPH_SELECT:
            return glyph_pill(x, cy, "SELECT", alpha);
        case GLYPH_L:
            return glyph_pill(x, cy, "L", alpha);
        case GLYPH_R:
            return glyph_pill(x, cy, "R", alpha);
        case GLYPH_DPAD: {
            uint32_t c = ui_alpha(COL_TEXT, alpha);
            glyph_disc(cx, cy, r, alpha);
            ui_rect(cx - 2, cy - 7, 4, 14, c);
            ui_rect(cx - 7, cy - 2, 14, 4, c);
            return r * 2;
        }
    }
    return 0;
}

void ui_hints(const UiHint *hints, int count) {
    float total = 0;
    float widths[16];
    for (int i = 0; i < count && i < 16; i++) {
        float gw = hints[i].glyph == GLYPH_START ? ui_text_width(0.62f, "START") + 16
                 : hints[i].glyph == GLYPH_SELECT ? ui_text_width(0.62f, "SELECT") + 16
                 : hints[i].glyph == GLYPH_L || hints[i].glyph == GLYPH_R ? ui_text_width(0.62f, "L") + 16
                 : 22;
        widths[i] = gw + 8 + ui_text_width(0.8f, hints[i].label);
        total += widths[i] + (i ? 26 : 0);
    }
    float y = SCREEN_H - 36;
    ui_gradient_v(0, SCREEN_H - 64, SCREEN_W, 64, RGBA8(0, 0, 0, 0), RGBA8(0, 0, 0, 170));
    float x = SCREEN_W - 32 - total;
    for (int i = 0; i < count && i < 16; i++) {
        float gw = ui_glyph(hints[i].glyph, x, y + 10, 1.0f);
        ui_text(x + gw + 8, y, COL_TEXT_DIM, 0.8f, hints[i].label);
        x += widths[i] + 26;
    }
}

void ui_background(void) {
    float t = ui_time();
    ui_gradient_v(0, 0, SCREEN_W, 300, RGBA8(6, 0, 3, 255), RGBA8(34, 2, 10, 255));
    ui_gradient_v(0, 300, SCREEN_W, 244, RGBA8(18, 0, 6, 255), RGBA8(4, 0, 2, 255));
    float hy = 300;
    float vx = SCREEN_W / 2;
    ui_gradient_v(0, hy - 60, SCREEN_W, 60, RGBA8(255, 30, 60, 0), RGBA8(255, 30, 60, 38));
    ui_rect(0, hy - 1, SCREEN_W, 2, RGBA8(255, 50, 80, 120));
    for (int i = -14; i <= 14; i++) {
        float bx = vx + i * 110.0f;
        float a = 1.0f - fabsf((float)i) / 15.0f;
        ui_line(vx + i * 6.0f, hy, bx, SCREEN_H, RGBA8(255, 30, 60, (int)(30 + 50 * a)));
    }
    float scroll = fmodf(t * 0.35f, 1.0f);
    for (int i = 0; i < 14; i++) {
        float z = (i + scroll) / 14.0f;
        float y = hy + (SCREEN_H - hy) * z * z;
        ui_line(0, y, SCREEN_W, y, RGBA8(255, 30, 60, (int)(20 + 90 * z)));
    }
    for (int i = 0; i < 40; i++) {
        float sx = fmodf(i * 241.7f, SCREEN_W);
        float sy = fmodf(i * 97.3f, hy - 80) + 70;
        float tw = 0.5f + 0.5f * sinf(t * (1.0f + (i % 5) * 0.3f) + i);
        ui_rect(sx, sy, 2, 2, RGBA8(255, 90, 110, (int)(30 + 80 * tw)));
    }
    ui_gradient_h(0, 0, 140, SCREEN_H, RGBA8(0, 0, 0, 140), RGBA8(0, 0, 0, 0));
    ui_gradient_h(SCREEN_W - 140, 0, 140, SCREEN_H, RGBA8(0, 0, 0, 0), RGBA8(0, 0, 0, 140));
}

static void draw_battery(float x, float y) {
    int pct = scePowerGetBatteryLifePercent();
    bool charging = scePowerIsBatteryCharging();
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    uint32_t c = pct <= 15 && !charging ? COL_ACCENT : COL_TEXT;
    ui_round_outline(x, y, 34, 18, 4, ui_alpha(c, 0.8f));
    ui_rect(x + 34, y + 5, 3, 8, ui_alpha(c, 0.8f));
    float fill = 28.0f * pct / 100.0f;
    ui_round_rect(x + 3, y + 3, fill, 12, 2, charging ? COL_OK : c);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    ui_text_right(x - 8, y - 1, COL_TEXT_DIM, 0.8f, buf);
}

static float draw_logo(float x, float y, float h) {
    if (!logo_tex) {
        float dot = h / 7.0f;
        ui_dot_text(x, y, dot, COL_ACCENT, "RED VIPER");
        return ui_dot_text_width(dot, "RED VIPER");
    }
    float s = h / logo_tex->h;
    float w = logo_tex->w * s;
    ui_draw_texture(logo_tex, x + 1, y + 2, w, h, RGBA8(0, 0, 0, 160));
    ui_draw_texture(logo_tex, x, y, w, h, COL_ACCENT);
    return w;
}

void ui_header(const char *subtitle) {
    ui_gradient_v(0, 0, SCREEN_W, 90, RGBA8(0, 0, 0, 190), RGBA8(0, 0, 0, 0));
    float lw = draw_logo(32, 16, 40);
    if (subtitle) {
        ui_rect(32 + lw + 18, 26, 2, 22, RGBA8(255, 38, 64, 120));
        ui_text(32 + lw + 32, 25, COL_TEXT_DIM, 0.9f, subtitle);
    }
    SceDateTime dt;
    sceRtcGetCurrentClockLocalTime(&dt);
    char clock[16];
    snprintf(clock, sizeof(clock), "%02d:%02d", dt.hour, dt.minute);
    float cw = ui_text_width(0.95f, clock);
    ui_text(SCREEN_W - 32 - cw, 24, COL_TEXT, 0.95f, clock);
    draw_battery(SCREEN_W - 32 - cw - 64, 27);
    ui_gradient_h(32, 66, SCREEN_W - 64, 1, RGBA8(255, 38, 64, 140), RGBA8(255, 38, 64, 0));
}

void ui_toast(const char *msg) {
    strncpy(toast_msg, msg, sizeof(toast_msg) - 1);
    toast_msg[sizeof(toast_msg) - 1] = 0;
    toast_born = ui_time();
    toast_until = ui_time() + 2.2f;
}

void ui_draw_toast(void) {
    float now = ui_time();
    if (now > toast_until) return;
    float in = ui_ease_out((now - toast_born) / 0.25f);
    float out = (toast_until - now) / 0.35f;
    float a = in < out ? in : out;
    if (a > 1) a = 1;
    float w = ui_text_width(0.9f, toast_msg) + 56;
    float x = (SCREEN_W - w) / 2;
    float y = SCREEN_H - 110 + (1 - in) * 20;
    ui_round_rect(x + 2, y + 4, w, 40, 20, ui_alpha(COL_SHADOW, a));
    ui_round_rect(x, y, w, 40, 20, ui_alpha(COL_PANEL_SOLID, a));
    ui_round_outline(x, y, w, 40, 20, ui_alpha(COL_BORDER, a * 2));
    ui_circle(x + 22, y + 20, 4, ui_alpha(COL_ACCENT, a));
    ui_text(x + 36, y + 10, ui_alpha(COL_TEXT, a), 0.9f, toast_msg);
}

void ui_icon_folder(float x, float y, uint32_t c) {
    ui_round_rect(x, y + 3, 12, 6, 2, c);
    ui_round_rect(x, y + 6, 24, 16, 3, c);
    ui_rect(x + 2, y + 10, 20, 1.5f, RGBA8(0, 0, 0, 70));
}

void ui_icon_file(float x, float y, float scale, uint32_t c) {
    float w = 18 * scale, h = 24 * scale, f = 6 * scale;
    UiVert v[5];
    set_v(&v[0], x, y, c);
    set_v(&v[1], x + w - f, y, c);
    set_v(&v[2], x + w, y + f, c);
    set_v(&v[3], x + w, y + h, c);
    set_v(&v[4], x, y + h, c);
    submit(PRIM_FAN, v, 5, 0, TEXMODE_SOLID);
    uint32_t shade = RGBA8(0, 0, 0, 90);
    UiVert t[3];
    set_v(&t[0], x + w - f, y, shade);
    set_v(&t[1], x + w - f, y + f, shade);
    set_v(&t[2], x + w, y + f, shade);
    submit(PRIM_TRIS, t, 3, 0, TEXMODE_SOLID);
    for (int i = 0; i < 3; i++)
        ui_rect(x + 4 * scale, y + (11 + i * 4) * scale, (i == 2 ? 6 : 10) * scale, 1.5f * scale, RGBA8(0, 0, 0, 90));
}

void ui_icon_drive(float x, float y, uint32_t c) {
    ui_round_rect(x, y + 5, 24, 14, 3, c);
    ui_circle(x + 18, y + 12, 2, RGBA8(0, 0, 0, 120));
    ui_rect(x + 4, y + 11, 8, 2, RGBA8(0, 0, 0, 110));
}

void ui_spinner(float cx, float cy, float r, uint32_t c) {
    float t = ui_time() * 6.0f;
    for (int i = 0; i < 10; i++) {
        float a = t + i * (2 * M_PI / 10);
        float alpha = (float)(i + 1) / 10.0f;
        ui_circle(cx + cosf(a) * r, cy + sinf(a) * r, 3.2f, ui_alpha(c, alpha));
    }
}
