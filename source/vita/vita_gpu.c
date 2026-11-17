#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v810_mem.h"
#include "vb_dsp.h"
#include "vb_set.h"
#include "video_hard_internal.h"
#include "vita.h"
#include "vita_color.h"
#include "vita_colorpack.h"

#define BASE_ROWS (CP_BASE_SLOTS / 32)
#define UNIFORM_CACHE 96

GLuint transparentPixelTexture;
static GLuint tileTexture;
static u16 *tileTextureBuffer = NULL;
static GLuint colorTexture;
static uint32_t *colorTextureData = NULL;
static GLuint varShadeTexture;
static u16 *varShadeData = NULL;
static GLuint varColorTexture;
static uint32_t *varColorData = NULL;
static int shade_dirty_min = BASE_ROWS, shade_dirty_max = -1;
static struct {
    GLuint prog;
    const char *name;
    GLint loc;
} uniform_cache[UNIFORM_CACHE];
static int uniform_count = 0;
static uint32_t *readback_buffer = NULL;
static int target_fb = 0;
static bool target_cache = false;
static int bk_of_fb[2];
static float shade_map[8][3];
static float flash_map[4];
GLuint screenTexHard[2];
GLuint screenTargetHard[2];
GLuint screenTexSoft[2];
static u16 *screenTexSoftBuffer = NULL;

GLuint eyeTex[2];
static GLuint eyeTarget[2];

static GLuint sChar, sFinal, sAffine;
static bool gpu_ready = false;
static bool shader_failed = false;
static int last_displayed_fb = 0;
static int last_vip_fb = 0;

static bool tiles_opaque = false;

static GLint uloc(GLuint prog, const char *name) {
    for (int i = 0; i < uniform_count; i++)
        if (uniform_cache[i].name == name && uniform_cache[i].prog == prog) return uniform_cache[i].loc;
    GLint loc = glGetUniformLocation(prog, name);
    if (uniform_count < UNIFORM_CACHE) {
        uniform_cache[uniform_count].prog = prog;
        uniform_cache[uniform_count].name = name;
        uniform_cache[uniform_count].loc = loc;
        uniform_count++;
    }
    return loc;
}

static GLuint compile(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    const char *sources[2] = {"#version 100\nprecision mediump float;\n", src};
    glShaderSource(sh, 2, sources, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        shader_failed = true;
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

GLuint vita_build_program(const char *vs, const char *fs, const char *const *attribs, int attrib_count) {
    if (shader_failed) return 0;
    GLuint v = compile(GL_VERTEX_SHADER, vs);
    GLuint f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) {
        if (v) glDeleteShader(v);
        if (f) glDeleteShader(f);
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    for (int i = 0; i < attrib_count; i++) glBindAttribLocation(p, i, attribs[i]);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) shader_failed = true;
    return p;
}

bool vita_shaders_failed(void) {
    return shader_failed;
}

static const char *const char_attribs[] = {"aPosition", "aParams"};
static const char *const affine_attribs[] = {"aOrigins", "aOffsets"};
static const char *const final_attribs[] = {"aPosition", "aTexCoord"};

static GLuint make_texture(int w, int h, GLenum type, bool repeat) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, type, NULL);
    return t;
}

static GLuint make_target(GLuint tex) {
    GLuint fb;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    return fb;
}

void gpu_init(void) {
    if (shader_failed) return;
    sChar = vita_build_program(
        "uniform vec2 uPosOffset;\n"
        "uniform vec3 uShades[8];\n"
        "uniform vec3 uRamps[15];\n"
        "uniform vec4 uFlash;\n"
        "uniform float uFigure;\n"
        "attribute vec2 aPosition;\n"
        "attribute vec4 aParams;\n"
        "varying vec4 vParams;\n"
        "varying vec3 vShades;\n"
        "varying vec3 vRamp0;\n"
        "varying vec3 vRamp1;\n"
        "varying vec3 vRamp2;\n"
        "void main() {\n"
        "   float pal = aParams.z;\n"
        "   vShades = uShades[int(pal)];\n"
        "   float sp = pal >= 4.0 ? pal - 4.0 : pal;\n"
        "   int r = (pal >= 4.0 || uFigure > 0.5) ? int(sp) * 3 : 12;\n"
        "   vRamp0 = uRamps[r];\n"
        "   vRamp1 = uRamps[r + 1];\n"
        "   vRamp2 = uRamps[r + 2];\n"
        "   vec4 sel = vec4(1.0) - min(abs(vec4(sp) - vec4(0.0, 1.0, 2.0, 3.0)), vec4(1.0));\n"
        "   float flash = pal >= 4.0 ? dot(sel, uFlash) : 0.0;\n"
        "   vParams = vec4(aParams.x, aParams.y, aParams.w, flash);\n"
        "   vec2 realPos = aPosition * (1.0 / 256.0) - (1.0 - 4.0 / 256.0) + uPosOffset;\n"
        "   gl_Position = vec4(realPos.y, realPos.x, 0.0, 1.0);\n"
        "   gl_PointSize = 8.0;\n"
        "}\n",
        "uniform sampler2D sTex;\n"
        "uniform sampler2D sColor;\n"
        "uniform sampler2D sVarTex;\n"
        "uniform sampler2D sVarColor;\n"
        "uniform float uOpaque;\n"
        "uniform float uMode;\n"
        "uniform float uFill;\n"
        "varying vec4 vParams;\n"
        "varying vec3 vShades;\n"
        "varying vec3 vRamp0;\n"
        "varying vec3 vRamp1;\n"
        "varying vec3 vRamp2;\n"
        "void main() {\n"
        "   vec2 tileCoord = gl_PointCoord.yx;\n"
        "   if (vParams.z >= 2.0) tileCoord.x = 1.0 - tileCoord.x;\n"
        "   if (vParams.z == 1.0 || vParams.z == 3.0) tileCoord.y = 1.0 - tileCoord.y;\n"
        "   bool variant = vParams.y >= 64.0;\n"
        "   vec2 realCoord = (vec2(vParams.x, variant ? vParams.y - 64.0 : vParams.y) + tileCoord) / vec2(32.0, 64.0);\n"
        "   vec4 t;\n"
        "   if (variant) t = texture2D(sVarTex, realCoord);\n"
        "   else t = texture2D(sTex, realCoord);\n"
        "   if (uFill > 0.5) {\n"
        "       if (t.a > 0.5) discard;\n"
        "       vec4 f;\n"
        "       if (variant) f = texture2D(sVarColor, realCoord);\n"
        "       else f = texture2D(sColor, realCoord);\n"
        "       if (f.a < 0.5) discard;\n"
        "       gl_FragColor = vec4(max(f.rgb, vec3(0.004)), 1.0);\n"
        "       return;\n"
        "   }\n"
        "   if (uOpaque < 0.5 && t.a < 0.5) discard;\n"
        "   float s = dot(t.rgb, vShades);\n"
        "   vec3 oh = vec3(1.0) - min(abs(vec3(s) - vec3(1.0, 2.0, 3.0)), vec3(1.0));\n"
        "   if (uMode < 0.5) {\n"
        "       gl_FragColor = vec4(oh, t.a);\n"
        "       return;\n"
        "   }\n"
        "   if (uMode < 1.5) {\n"
        "       gl_FragColor = vec4(0.0, s / 16.0, 0.0, t.a);\n"
        "       return;\n"
        "   }\n"
        "   if (t.a < 0.5) {\n"
        "       gl_FragColor = vec4(0.0);\n"
        "       return;\n"
        "   }\n"
        "   if (s < 0.5) {\n"
        "       gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);\n"
        "       return;\n"
        "   }\n"
        "   vec4 pc;\n"
        "   if (variant) pc = texture2D(sVarColor, realCoord);\n"
        "   else pc = texture2D(sColor, realCoord);\n"
        "   if (pc.a > 0.5) {\n"
        "       vec3 c = pc.rgb;\n"
        "       if (vParams.w > 0.5) c = mix(c, vec3(step(1.5, s) * 0.5 + step(2.5, s) * 0.5), 0.6);\n"
        "       gl_FragColor = vec4(max(c, vec3(0.004)), 1.0);\n"
        "       return;\n"
        "   }\n"
        "   if (uMode > 2.5) {\n"
        "       gl_FragColor = vec4(oh, 0.75);\n"
        "       return;\n"
        "   }\n"
        "   gl_FragColor = vec4(vRamp0 * oh.x + vRamp1 * oh.y + vRamp2 * oh.z, 1.0);\n"
        "}\n",
        char_attribs, 2);

    sAffine = vita_build_program(
        "uniform float uRight;\n"
        "uniform vec2 uPosOffset;\n"
        "attribute vec4 aOrigins;\n"
        "attribute vec4 aOffsets;\n"
        "varying vec2 vTexCoord;\n"
        "void main() {\n"
        "   float texOffset = aOffsets.w;\n"
        "   if ((uRight > 0.5) != (aOffsets.z < 0.0)) texOffset += abs(aOffsets.z);\n"
        "   vec2 posOffset = vec2(0.5 - 1.0 / 512.0);\n"
        "   gl_Position = vec4((aOrigins.yx + uPosOffset.yx + posOffset.yx) / 256.0 - 1.0, 0.0, 1.0);\n"
        "   vTexCoord = (aOrigins.zw + (aOffsets.xy * texOffset) / 64.0) / 4096.0;\n"
        "}\n",
        "uniform sampler2D sTex;\n"
        "uniform float uRepeat;\n"
        "uniform float uStartMap;\n"
        "uniform vec3 uWorldSize;\n"
        "uniform vec4 uMapVisibleA;\n"
        "uniform vec4 uMapVisibleB;\n"
        "uniform float uColor;\n"
        "uniform vec3 uRamp[3];\n"
        "varying vec2 vTexCoord;\n"
        "void main() {\n"
        "   vec2 texCoord = vTexCoord;\n"
        "   vec2 wrapped = mod(texCoord, uWorldSize.xy);\n"
        "   if (uRepeat < 0.5 && (wrapped.x != texCoord.x || wrapped.y != texCoord.y)) discard;\n"
        "   texCoord = mod(texCoord, uWorldSize.zy);\n"
        "   float sub_map = floor(texCoord.x) + floor(texCoord.y) * uWorldSize.x;\n"
        "   vec4 selA = vec4(1.0) - min(abs(vec4(sub_map) - vec4(0.0, 1.0, 2.0, 3.0)), vec4(1.0));\n"
        "   vec4 selB = vec4(1.0) - min(abs(vec4(sub_map) - vec4(4.0, 5.0, 6.0, 7.0)), vec4(1.0));\n"
        "   float visible = dot(selA, uMapVisibleA) + dot(selB, uMapVisibleB);\n"
        "   if (visible < 0.5) discard;\n"
        "   vec2 mapOffset = vec2(mod(floor(uStartMap + sub_map), 4.0), floor((uStartMap + sub_map) / 4.0));\n"
        "   vec4 c = texture2D(sTex, (mod(texCoord.yx, 1.0) + mapOffset) * vec2(0.25, 0.5));\n"
        "   if (c.a < 0.5) discard;\n"
        "   if (uColor > 0.5 && c.a < 0.9) c = vec4(uRamp[0] * c.r + uRamp[1] * c.g + uRamp[2] * c.b, 1.0);\n"
        "   gl_FragColor = c;\n"
        "}\n",
        affine_attribs, 2);

    sFinal = vita_build_program(
        "attribute vec2 aPosition;\n"
        "attribute vec2 aTexCoord;\n"
        "varying vec2 vTexCoord;\n"
        "void main() {\n"
        "   gl_Position = vec4(aPosition, 0.0, 1.0);\n"
        "   vTexCoord = aTexCoord;\n"
        "}\n",
        "uniform sampler2D sVip;\n"
        "uniform sampler2D sSoft;\n"
        "uniform vec3 uPalette[4];\n"
        "uniform float uVipOverSoft;\n"
        "uniform float uColor;\n"
        "uniform vec3 uBg;\n"
        "uniform vec3 uBk;\n"
        "uniform float uBkLit;\n"
        "uniform float uFade;\n"
        "varying vec2 vTexCoord;\n"
        "void main() {\n"
        "   vec4 vip = texture2D(sVip, vTexCoord);\n"
        "   vec4 soft = texture2D(sSoft, vTexCoord);\n"
        "   if (uColor > 0.5) {\n"
        "       vec3 vc = vip.a > 0.5 ? vip.rgb : uBk;\n"
        "       float vl = vip.a > 0.5 ? step(0.002, vip.r + vip.g + vip.b) : uBkLit;\n"
        "       float uv = uVipOverSoft > 0.5 ? vl : 1.0 - step(0.5, soft.a);\n"
        "       vec3 sc = mix(mix(mix(uPalette[0], uPalette[1], soft.r), uPalette[2], soft.g), uPalette[3], soft.b);\n"
        "       gl_FragColor = vec4(mix(uBg, mix(sc, vc, uv), uFade), 1.0);\n"
        "       return;\n"
        "   }\n"
        "   float vipLit = step(0.01, vip.r + vip.g + vip.b);\n"
        "   float useVip = uVipOverSoft > 0.5 ? vipLit : 1.0 - step(0.5, soft.a);\n"
        "   vec4 color = mix(soft, vip, useVip);\n"
        "   vec3 c = mix(mix(mix(uPalette[0], uPalette[1], color.r), uPalette[2], color.g), uPalette[3], color.b);\n"
        "   gl_FragColor = vec4(c, 1.0);\n"
        "}\n",
        final_attribs, 2);

    transparentPixelTexture = make_texture(1, 1, GL_UNSIGNED_SHORT_4_4_4_4, true);
    u16 pixel = 0;
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, &pixel);

    uniform_count = 0;
    tileTexture = make_texture(256, BASE_ROWS * 8, GL_UNSIGNED_SHORT_4_4_4_4, false);
    if (tileTextureBuffer == NULL) tileTextureBuffer = calloc(256 * BASE_ROWS * 8, sizeof(tileTextureBuffer[0]));
    colorTexture = make_texture(256, BASE_ROWS * 8, GL_UNSIGNED_BYTE, false);
    colorTextureData = vglGetTexDataPointer(GL_TEXTURE_2D);
    if (colorTextureData) memset(colorTextureData, 0, 256 * BASE_ROWS * 8 * 4);
    varShadeTexture = make_texture(256, BASE_ROWS * 8, GL_UNSIGNED_SHORT_4_4_4_4, false);
    varShadeData = vglGetTexDataPointer(GL_TEXTURE_2D);
    if (varShadeData) memset(varShadeData, 0, 256 * BASE_ROWS * 8 * 2);
    varColorTexture = make_texture(256, BASE_ROWS * 8, GL_UNSIGNED_BYTE, false);
    varColorData = vglGetTexDataPointer(GL_TEXTURE_2D);
    if (varColorData) memset(varColorData, 0, 256 * BASE_ROWS * 8 * 4);
    if (readback_buffer == NULL) readback_buffer = calloc(384 * DOWNLOADED_FRAMEBUFFER_WIDTH * 2, sizeof(readback_buffer[0]));

    for (int i = 0; i < 2; i++) {
        screenTexHard[i] = make_texture(512, 512, GL_UNSIGNED_BYTE, false);
        screenTargetHard[i] = make_target(screenTexHard[i]);
        screenTexSoft[i] = make_texture(512, 512, GL_UNSIGNED_SHORT_4_4_4_4, false);
        eyeTex[i] = make_texture(VB_W, VB_H, GL_UNSIGNED_BYTE, false);
        eyeTarget[i] = make_target(eyeTex[i]);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    if (screenTexSoftBuffer == NULL) screenTexSoftBuffer = calloc(512 * 512, sizeof(screenTexSoftBuffer[0]));

    GLuint affine_tex = make_texture(512 * 4, 512 * 2, GL_UNSIGNED_BYTE, true);
    GLuint affine_target = make_target(affine_tex);
    for (int i = 0; i < AFFINE_CACHE_SIZE; i++) {
        tileMapCache[i].tex = affine_tex;
        tileMapCache[i].target = affine_target;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    gpu_ready = true;
}

void gpu_quit(void) {
    if (!gpu_ready) return;
    glDeleteProgram(sChar);
    glDeleteProgram(sAffine);
    glDeleteProgram(sFinal);
    glDeleteTextures(1, &transparentPixelTexture);
    glDeleteTextures(1, &tileTexture);
    glDeleteTextures(1, &colorTexture);
    glDeleteTextures(1, &varShadeTexture);
    glDeleteTextures(1, &varColorTexture);
    colorTextureData = NULL;
    varShadeData = NULL;
    varColorData = NULL;
    glDeleteTextures(2, screenTexHard);
    glDeleteFramebuffers(2, screenTargetHard);
    glDeleteTextures(2, screenTexSoft);
    glDeleteTextures(2, eyeTex);
    glDeleteFramebuffers(2, eyeTarget);
    glDeleteTextures(1, &tileMapCache[0].tex);
    glDeleteFramebuffers(1, &tileMapCache[0].target);
    free(tileTextureBuffer);
    tileTextureBuffer = NULL;
    free(readback_buffer);
    readback_buffer = NULL;
    free(screenTexSoftBuffer);
    screenTexSoftBuffer = NULL;
    gpu_ready = false;
}

void gpu_clear_screen(bool outside_rendering) {
    (void)outside_rendering;
    glClearColor(0, 0, 0, 0);
    for (int i = 0; i < 2; i++) {
        gpu_target_screen(i);
        glClear(GL_COLOR_BUFFER_BIT);
    }
}

void gpu_setup_screen(int start_eye, int end_eye) {
    (void)start_eye;
    (void)end_eye;
    int bk = vb_state->tVIPREG.BKCOL & 3;
    bk_of_fb[target_fb & 1] = bk;
    glDisable(GL_SCISSOR_TEST);
    if (cp_frame_on) {
        const CpShading *sh = cp_shading();
        glClearColor(sh->base[bk][0], sh->base[bk][1], sh->base[bk][2], 0);
    } else {
        glClearColor(bk == 1, bk == 2, bk == 3, bk != 0);
    }
    glClear(GL_COLOR_BUFFER_BIT);
}

void gpu_setup_drawing(void) {
    bool off[4] = {true, false, false, false};
    if (cp_frame_on)
        for (int s = 1; s < 4; s++) off[s] = cp_shade_off(s);
    for (int i = 0; i < 8; i++) {
        HWORD pal = i < 4 ? vb_state->tVIPREG.GPLT[i] : vb_state->tVIPREG.JPLT[i - 4];
        int raw[3];
        for (int j = 0; j < 3; j++) {
            raw[j] = (pal >> (2 * (j + 1))) & 3;
            shade_map[i][j] = off[raw[j]] && cp_frame_on ? 0.0f : (float)raw[j];
        }
        if (i >= 4) flash_map[i - 4] = raw[0] == raw[1] && raw[1] == raw[2] ? 1.0f : 0.0f;
    }
}

static void flush_slots(void) {
    if (shade_dirty_min <= shade_dirty_max) {
        glBindTexture(GL_TEXTURE_2D, tileTexture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, shade_dirty_min * 8, 256, (shade_dirty_max - shade_dirty_min + 1) * 8, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, tileTextureBuffer + shade_dirty_min * 256 * 8);
        shade_dirty_min = BASE_ROWS;
        shade_dirty_max = -1;
    }
}

static void mark_shade(int slot) {
    int row = slot / 32;
    if (row < shade_dirty_min) shade_dirty_min = row;
    if (row > shade_dirty_max) shade_dirty_max = row;
}

static void write_colors(uint32_t *base, int slot, const uint32_t colors[64]) {
    uint32_t *dst = base + (slot / 32) * 256 * 8 + (slot % 32) * 8;
    for (int y = 0; y < 8; y++) memcpy(dst + 256 * y, colors + y * 8, 8 * sizeof(uint32_t));
}

void gpu_slot_colors(int slot, const uint32_t colors[64]) {
    if (!colorTextureData || slot < 0 || slot >= CP_BASE_SLOTS) return;
    write_colors(colorTextureData, slot, colors);
}

void gpu_slot_variant(int slot, int src_chr, const uint32_t colors[64]) {
    if (!varShadeData || !varColorData || slot < CP_BASE_SLOTS || slot >= CP_TOTAL_SLOTS) return;
    int v = slot - CP_BASE_SLOTS;
    u16 *dst = varShadeData + (v / 32) * 256 * 8 + (v % 32) * 8;
    const u16 *src = tileTextureBuffer + (src_chr / 32) * 256 * 8 + (src_chr % 32) * 8;
    for (int y = 0; y < 8; y++) memcpy(dst + 256 * y, src + 256 * y, 8 * sizeof(u16));
    write_colors(varColorData, v, colors);
}

void gpu_reset_color_state(void) {
    for (int i = 0; i < AFFINE_CACHE_SIZE; i++) tileMapCache[i].bg = -1;
}

void video_color_refresh(bool game_loaded) {
    cp_invalidate();
    gpu_reset_color_state();
    if (!game_loaded) return;
    clearCache();
    video_rerender();
}

static void bind_tile_textures(void) {
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, varColorTexture);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, varShadeTexture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, colorTexture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tileTexture);
}

void gpu_setup_tile_drawing(void) {
    glUseProgram(sChar);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_SHORT, GL_FALSE, sizeof(vertex), vbuf);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(vertex), (u8 *)vbuf + 4);
    glUniform3fv(uloc(sChar, "uShades"), 8, &shade_map[0][0]);
    glUniform4fv(uloc(sChar, "uFlash"), 1, flash_map);
    glUniform1i(uloc(sChar, "sTex"), 0);
    glUniform1i(uloc(sChar, "sColor"), 1);
    glUniform1i(uloc(sChar, "sVarTex"), 2);
    glUniform1i(uloc(sChar, "sVarColor"), 3);
    bind_tile_textures();
}

void gpu_set_tile_offset(float xoffset, float yoffset) {
    glUniform2f(uloc(sChar, "uPosOffset"), xoffset, yoffset);
}

void gpu_target_screen(int drawn_fb) {
    target_fb = drawn_fb;
    target_cache = false;
    glBindFramebuffer(GL_FRAMEBUFFER, screenTargetHard[drawn_fb]);
    glViewport(0, 0, 512, 512);
}

void gpu_target_affine(int cache_id) {
    target_cache = true;
    glBindFramebuffer(GL_FRAMEBUFFER, tileMapCache[cache_id].target);
    int xoff = cache_id % 4;
    int yoff = cache_id / 4;
    glViewport(512 * xoff, 512 * yoff, 512, 512);
    gpu_set_scissor(true, 512 * xoff, 512 * yoff, 512 * (xoff + 1), 512 * (yoff + 1));
}

void gpu_set_scissor(bool enabled, u32 left, u32 top, u32 right, u32 bottom) {
    if (enabled) glEnable(GL_SCISSOR_TEST);
    else glDisable(GL_SCISSOR_TEST);
    if (enabled && right > left && bottom > top) glScissor(left, top, right - left, bottom - top);
    else if (enabled) glScissor(0, 0, 0, 0);
}

void gpu_set_opaque(bool opaque) {
    tiles_opaque = opaque;
    glDisable(GL_BLEND);
}

static void set_tile_uniforms(void) {
    flush_slots();
    bind_tile_textures();
    float mode = tVBOpt.RENDERMODE == RM_TOCPU ? 1.0f : cp_frame_on ? (target_cache ? 3.0f : 2.0f) : 0.0f;
    glUniform1f(uloc(sChar, "uOpaque"), tiles_opaque ? 1.0f : 0.0f);
    glUniform1f(uloc(sChar, "uMode"), mode);
    if (cp_frame_on) {
        const CpShading *sh = cp_shading();
        float ramps[15][3];
        memcpy(ramps, sh->sprite_ramps, sizeof(sh->sprite_ramps));
        memcpy(ramps[12], sh->world_ramp, sizeof(sh->world_ramp));
        glUniform3fv(uloc(sChar, "uRamps"), 15, &ramps[0][0]);
        glUniform1f(uloc(sChar, "uFigure"), sh->figure ? 1.0f : 0.0f);
    }
}

void gpu_draw_tiles(int first, int count) {
    set_tile_uniforms();
    glUniform1f(uloc(sChar, "uFill"), 0.0f);
    glDrawArrays(GL_POINTS, first, count);
}

void gpu_draw_fills(int first, int count) {
    if (!cp_frame_on || target_cache) return;
    set_tile_uniforms();
    glUniform1f(uloc(sChar, "uFill"), 1.0f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE_MINUS_DST_ALPHA, GL_DST_ALPHA);
    glDrawArrays(GL_POINTS, first, count);
    glDisable(GL_BLEND);
    glUniform1f(uloc(sChar, "uFill"), 0.0f);
}

bool gpu_can_chunk(void) {
    return false;
}

void gpu_draw_affine(WORLD *world, int umin, int vmin, int umax, int vmax, int drawn_fb, bool visible[]) {
    (void)umin;
    (void)vmin;
    (void)umax;
    (void)vmax;
    (void)drawn_fb;
    uint8_t mapid = world->head & 0xf;
    uint8_t scx_pow = ((world->head >> 10) & 3);
    uint8_t scy_pow = ((world->head >> 8) & 3);
    uint8_t map_count_pow = scx_pow + scy_pow;
    bool huge_bg = map_count_pow > 3;
    if (huge_bg) map_count_pow = 3;
    uint8_t scx = 1 << scx_pow;
    uint8_t scy = 1 << scy_pow;
    uint8_t map_count = 1 << map_count_pow;
    mapid &= ~(map_count - 1);
    bool over = world->head & 0x80;
    int16_t base_gx = (s16)(world->gx << 6) >> 6;
    int16_t gp = (s16)(world->gp << 6) >> 6;
    int16_t gy = world->gy;
    int16_t w = world->w + 1;
    int16_t h = world->h + 1;

    glUseProgram(sAffine);
    avertex *current_avbuf = avcur - h;
    int vertex_size = sizeof(avertex) / 2;
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_SHORT, GL_FALSE, vertex_size, current_avbuf);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_SHORT, GL_FALSE, vertex_size, (u8 *)current_avbuf + offsetof(avertex, dx1));

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tileMapCache[0].tex);
    glUniform1i(uloc(sAffine, "sTex"), 0);
    glUniform1f(uloc(sAffine, "uRepeat"), over ? 0.0f : 1.0f);
    glUniform1f(uloc(sAffine, "uStartMap"), mapid % AFFINE_CACHE_SIZE);
    glUniform3f(uloc(sAffine, "uWorldSize"), scx, scy, huge_bg ? 8 / scy : scx);
    float vis[8] = {0};
    for (int sub_bg = 0; sub_bg < map_count; sub_bg++) {
        int cache_id = (mapid + sub_bg) % AFFINE_CACHE_SIZE;
        vis[sub_bg] = (visible[sub_bg] && tileMapCache[cache_id].bg == mapid + sub_bg && tileMapCache[cache_id].visible) ? 1.0f : 0.0f;
    }
    glUniform4fv(uloc(sAffine, "uMapVisibleA"), 1, vis);
    glUniform4fv(uloc(sAffine, "uMapVisibleB"), 1, vis + 4);
    glUniform1f(uloc(sAffine, "uColor"), cp_frame_on ? 1.0f : 0.0f);
    if (cp_frame_on) {
        const CpShading *sh = cp_shading();
        glUniform3fv(uloc(sAffine, "uRamp"), 3, sh->figure ? &sh->sprite_ramps[0][0][0] : &sh->world_ramp[0][0]);
    }

    for (int eye = 0; eye < 2; eye++) {
        if (world->on & (2 >> eye)) {
            int gx = base_gx + (eye == 0 ? -gp : gp);
            glUniform1f(uloc(sAffine, "uRight"), (float)eye);
            glUniform2f(uloc(sAffine, "uPosOffset"), eye == 0 ? -gp : gp, eye * 256);
            gpu_set_scissor(true, 256 * eye + (gy >= 0 ? gy : 0), gx >= 0 ? gx : 0, (gy + h < 256 ? gy + h : 256) + 256 * eye, gx + w);
            glDrawArrays(GL_LINES, 0, h * 2);
        }
    }
}

void update_texture_cache_hard(void) {
    for (int t = 0; t < 2048; t++) {
        if (!tDSPCACHE.CharacterCache[t]) {
            if (blankTile < 0 && !tileVisible[t]) blankTile = t;
            continue;
        }
        u16 *tile = (u16 *)(vb_state->V810_DISPLAY_RAM.off + ((t & 0x600) << 6) + 0x6000 + (t & 0x1ff) * 16);
        u16 rows[8];
        memcpy(rows, tile, sizeof(rows));
        int ty = t / 32;
        int tx = t % 32;
        u16 *dstbuf = tileTextureBuffer + ty * 256 * 8 + tx * 8;
        bool tv = (rows[0] | rows[1] | rows[2] | rows[3] | rows[4] | rows[5] | rows[6] | rows[7]) != 0;
        tileVisible[t] = tv;
        mark_shade(t);
        cp_slot_changed(t, rows);
        if (!tv) {
            if (blankTile < 0) blankTile = t;
            for (int y = 0; y < 8; y++) memset(dstbuf + 256 * y, 0, 8 * 2);
            continue;
        }
        static const uint16_t colors[4] = {0, 0xf00f, 0x0f0f, 0x00ff};
        for (int y = 0; y < 8; y++) {
            u16 row = rows[y];
            for (int x = 0; x < 8; x++) {
                *dstbuf++ = colors[row & 3];
                row >>= 2;
            }
            dstbuf += 256 - 8;
        }
    }
    flush_slots();
}

void gpu_soft_to_texture(int displayed_fb) {
    if (tDSPCACHE.DDSPDataState[displayed_fb] == CPU_WROTE) {
        tDSPCACHE.DDSPDataState[displayed_fb] = GPU_WROTE;
    } else {
        return;
    }
    static const uint16_t colors[4] = {0, 0xf00f, 0x0f0f, 0x00ff};
    for (int eye = 0; eye < 2; eye++) {
        for (int x = 0; x < 384; x++) {
            uint32_t *in_fb_ptr = (uint32_t *)(vb_state->V810_DISPLAY_RAM.off + 0x10000 * eye + 0x8000 * displayed_fb + x * (256 / 4));
            uint32_t *out_fb = (uint32_t *)(screenTexSoftBuffer + x * 512 + eye * 256);
            for (int y = 0; y < 224 / 16; y++) {
                uint32_t blob = *in_fb_ptr++;
                for (int i = 0; i < 32 / 4; i++) {
                    *out_fb++ = colors[blob & 3] | ((uint32_t)colors[(blob >> 2) & 3] << 16);
                    blob >>= 4;
                }
            }
        }
    }
    glBindTexture(GL_TEXTURE_2D, screenTexSoft[displayed_fb]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 512, 512, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, screenTexSoftBuffer);
}

static bool downloaded;

void gpu_reset_vip_download(void) {
}

void gpu_init_vip_download(int start_eye, int end_eye, int drawn_fb) {
    downloaded = false;
    glBindFramebuffer(GL_FRAMEBUFFER, screenTargetHard[tVBOpt.DOUBLE_BUFFER ? drawn_fb : 0]);
    for (int eye = start_eye; eye < end_eye; eye++) {
        glReadPixels(eye * 256, 0, DOWNLOADED_FRAMEBUFFER_WIDTH, 384, GL_RGBA, GL_UNSIGNED_BYTE, readback_buffer + (384 * DOWNLOADED_FRAMEBUFFER_WIDTH * eye));
    }
}

void video_download_vip(int drawn_fb) {
    if (tVBOpt.RENDERMODE != RM_TOCPU) return;
    if (downloaded) return;
    downloaded = true;
    for (int eye = 0; eye < 2; eye++) {
        const uint32_t *in_fb = readback_buffer + (384 * DOWNLOADED_FRAMEBUFFER_WIDTH) * eye;
        uint32_t *out_fb = (uint32_t *)(vb_state->V810_DISPLAY_RAM.off + 0x10000 * eye + 0x8000 * drawn_fb);
        for (int x = 0; x < 384; x++) {
            const uint32_t *row = in_fb + x * DOWNLOADED_FRAMEBUFFER_WIDTH;
            for (int y = 0; y < 224; y += 16) {
                uint32_t buf = 0;
                for (int i = 0; i < 16; i++) {
                    uint32_t g = (row[y + i] >> 8) & 0xFF;
                    uint32_t shade = (g + 8) >> 4;
                    if (shade > 3) shade = 3;
                    buf |= shade << (i * 2);
                }
                *out_fb++ = buf;
            }
            out_fb += (256 - 224) / 4 / sizeof(out_fb[0]);
        }
    }
    tDSPCACHE.DDSPDataState[drawn_fb] = CPU_WROTE;
    for (int i = 0; i < 64; i++) {
        tDSPCACHE.SoftBufWrote[drawn_fb][i].min = 0;
        tDSPCACHE.SoftBufWrote[drawn_fb][i].max = 31;
    }
}

void gpu_blend_antiflicker(void) {
}

void gpu_blend_default(void) {
}

bool gpu_antiflicker_allowed(void) {
    return false;
}

static void compose_eye(int eye, int displayed_fb, int vip_fb, const float pal[4][3]) {
    glBindFramebuffer(GL_FRAMEBUFFER, eyeTarget[eye]);
    glViewport(0, 0, VB_W, VB_H);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glUseProgram(sFinal);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tVBOpt.RENDERMODE != RM_TOCPU ? screenTexHard[vip_fb] : transparentPixelTexture);
    glUniform1i(uloc(sFinal, "sVip"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tDSPCACHE.DDSPDataState[displayed_fb] != GPU_CLEAR ? screenTexSoft[displayed_fb] : transparentPixelTexture);
    glUniform1i(uloc(sFinal, "sSoft"), 1);
    glActiveTexture(GL_TEXTURE0);
    glUniform1f(uloc(sFinal, "uVipOverSoft"), tVBOpt.VIP_OVER_SOFT ? 1.0f : 0.0f);
    if (cp_display_on()) {
        const CpShading *sh = cp_shading();
        int bk = bk_of_fb[vip_fb & 1];
        glUniform1f(uloc(sFinal, "uColor"), 1.0f);
        glUniform3fv(uloc(sFinal, "uPalette"), 4, &sh->base[0][0]);
        glUniform3fv(uloc(sFinal, "uBg"), 1, sh->base[0]);
        glUniform3fv(uloc(sFinal, "uBk"), 1, sh->base[bk]);
        glUniform1f(uloc(sFinal, "uBkLit"), bk != 0 && tVBOpt.RENDERMODE != RM_TOCPU ? 1.0f : 0.0f);
        glUniform1f(uloc(sFinal, "uFade"), cp_fade());
    } else {
        glUniform1f(uloc(sFinal, "uColor"), 0.0f);
        glUniform3fv(uloc(sFinal, "uPalette"), 4, &pal[0][0]);
    }
    static const GLfloat pos[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    float o = eye * 0.5f;
    GLfloat tc[] = {
        224.0f / 512.0f + o, 0,
        224.0f / 512.0f + o, 384.0f / 512.0f,
        0 + o, 0,
        0 + o, 384.0f / 512.0f,
    };
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, tc);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void vita_compose_eyes(void) {
    if (!gpu_ready) return;
    float pal[4][3];
    color_frame_palette(pal);
    for (int eye = 0; eye < 2; eye++) {
        bool wanted = settings.stereo == STEREO_ANAGLYPH || settings.stereo == STEREO_SIDE_BY_SIDE || (settings.stereo == STEREO_RIGHT) == (eye == 1);
        if (wanted) compose_eye(eye, last_displayed_fb, last_vip_fb, pal);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void gpu_flush(bool default_for_both, int displayed_fb, int vip_displayed_fb) {
    (void)default_for_both;
    last_displayed_fb = displayed_fb;
    last_vip_fb = vip_displayed_fb;
    vita_compose_eyes();
    gl_flush();
}

bool vita_read_eye(int eye, uint8_t *rgba) {
    if (!gpu_ready) return false;
    glBindFramebuffer(GL_FRAMEBUFFER, eyeTarget[eye & 1]);
    glReadPixels(0, 0, VB_W, VB_H, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}
