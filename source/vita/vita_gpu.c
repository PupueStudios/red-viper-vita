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

GLuint transparentPixelTexture;
static GLuint tileTexture;
static u16 *tileTextureBuffer = NULL;
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

static float palettes[8][3][3];
static bool tiles_opaque = false;

static GLuint compile(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    const char *sources[2] = {"#version 100\nprecision mediump float;\n", src};
    glShaderSource(sh, 2, sources, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) shader_failed = true;
    return sh;
}

GLuint vita_build_program(const char *vs, const char *fs, const char *const *attribs, int attrib_count) {
    GLuint v = compile(GL_VERTEX_SHADER, vs);
    GLuint f = compile(GL_FRAGMENT_SHADER, fs);
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    for (int i = 0; i < attrib_count; i++) glBindAttribLocation(p, i, attribs[i]);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) shader_failed = true;
    glDeleteShader(v);
    glDeleteShader(f);
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
    sChar = vita_build_program(
        "uniform vec2 uPosOffset;\n"
        "uniform vec3 uPalette[24];\n"
        "attribute vec2 aPosition;\n"
        "attribute vec4 aParams;\n"
        "varying vec3 vParams;\n"
        "varying vec3 vPal0;\n"
        "varying vec3 vPal1;\n"
        "varying vec3 vPal2;\n"
        "void main() {\n"
        "   vParams = aParams.xyw;\n"
        "   int p = int(aParams.z) * 3;\n"
        "   vPal0 = uPalette[p];\n"
        "   vPal1 = uPalette[p + 1];\n"
        "   vPal2 = uPalette[p + 2];\n"
        "   vec2 realPos = aPosition * (1.0 / 256.0) - (1.0 - 4.0 / 256.0) + uPosOffset;\n"
        "   gl_Position = vec4(realPos.y, realPos.x, 0.0, 1.0);\n"
        "   gl_PointSize = 8.0;\n"
        "}\n",
        "uniform sampler2D sTex;\n"
        "uniform float uOpaque;\n"
        "varying vec3 vParams;\n"
        "varying vec3 vPal0;\n"
        "varying vec3 vPal1;\n"
        "varying vec3 vPal2;\n"
        "void main() {\n"
        "   vec2 tileCoord = gl_PointCoord.yx;\n"
        "   if (vParams.z < 2.0) tileCoord.x = 1.0 - tileCoord.x;\n"
        "   if (vParams.z == 1.0 || vParams.z == 3.0) tileCoord.y = 1.0 - tileCoord.y;\n"
        "   vec2 realCoord = (vParams.xy + tileCoord) / vec2(32.0, 64.0);\n"
        "   vec4 color = texture2D(sTex, realCoord);\n"
        "   if (uOpaque < 0.5 && color.a < 0.5) discard;\n"
        "   gl_FragColor = vec4(vPal0 * color.r + vPal1 * color.g + vPal2 * color.b, color.a);\n"
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
        "varying vec2 vTexCoord;\n"
        "void main() {\n"
        "   vec4 vip = texture2D(sVip, vTexCoord);\n"
        "   vec4 soft = texture2D(sSoft, vTexCoord);\n"
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

    tileTexture = make_texture(256, 512, GL_UNSIGNED_SHORT_4_4_4_4, false);
    if (tileTextureBuffer == NULL) tileTextureBuffer = calloc(256 * 512, sizeof(tileTextureBuffer[0]));

    for (int i = 0; i < 2; i++) {
        screenTexHard[i] = make_texture(512, 512, GL_UNSIGNED_SHORT_4_4_4_4, false);
        screenTargetHard[i] = make_target(screenTexHard[i]);
        screenTexSoft[i] = make_texture(512, 512, GL_UNSIGNED_SHORT_4_4_4_4, false);
        eyeTex[i] = make_texture(VB_W, VB_H, GL_UNSIGNED_BYTE, false);
        eyeTarget[i] = make_target(eyeTex[i]);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    if (screenTexSoftBuffer == NULL) screenTexSoftBuffer = calloc(512 * 512, sizeof(screenTexSoftBuffer[0]));

    GLuint affine_tex = make_texture(512 * 4, 512 * 2, GL_UNSIGNED_SHORT_4_4_4_4, true);
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
    glDeleteTextures(2, screenTexHard);
    glDeleteFramebuffers(2, screenTargetHard);
    glDeleteTextures(2, screenTexSoft);
    glDeleteTextures(2, eyeTex);
    glDeleteFramebuffers(2, eyeTarget);
    glDeleteTextures(1, &tileMapCache[0].tex);
    glDeleteFramebuffers(1, &tileMapCache[0].target);
    free(tileTextureBuffer);
    tileTextureBuffer = NULL;
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
    glDisable(GL_SCISSOR_TEST);
    glClearColor(vb_state->tVIPREG.BKCOL == 1, vb_state->tVIPREG.BKCOL == 2, vb_state->tVIPREG.BKCOL == 3, vb_state->tVIPREG.BKCOL != 0);
    glClear(GL_COLOR_BUFFER_BIT);
}

void gpu_setup_drawing(void) {
    static const float normal_cols[4][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    static const float tocpu_cols[4][3] = {{0, 0, 0}, {0, 1.0 / 16, 0}, {0, 2.0 / 16, 0}, {0, 3.0 / 16, 0}};
    const float (*cols)[3] = (tVBOpt.RENDERMODE != RM_TOCPU) ? normal_cols : tocpu_cols;
    for (int i = 0; i < 4; i++) {
        HWORD pal = vb_state->tVIPREG.GPLT[i];
        for (int j = 0; j < 3; j++) {
            pal = pal >> 2;
            memcpy(palettes[i][j], cols[pal & 3], sizeof(cols[0]));
        }
        pal = vb_state->tVIPREG.JPLT[i];
        for (int j = 0; j < 3; j++) {
            pal = pal >> 2;
            memcpy(palettes[i + 4][j], cols[pal & 3], sizeof(cols[0]));
        }
    }
}

void gpu_setup_tile_drawing(void) {
    glUseProgram(sChar);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_SHORT, GL_FALSE, sizeof(vertex), vbuf);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(vertex), (u8 *)vbuf + 4);
    glUniform3fv(glGetUniformLocation(sChar, "uPalette"), 24, &palettes[0][0][0]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tileTexture);
    glUniform1i(glGetUniformLocation(sChar, "sTex"), 0);
}

void gpu_set_tile_offset(float xoffset, float yoffset) {
    glUniform2f(glGetUniformLocation(sChar, "uPosOffset"), xoffset, yoffset);
}

void gpu_target_screen(int drawn_fb) {
    glBindFramebuffer(GL_FRAMEBUFFER, screenTargetHard[drawn_fb]);
    glViewport(0, 0, 512, 512);
}

void gpu_target_affine(int cache_id) {
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

void gpu_draw_tiles(int first, int count) {
    glUniform1f(glGetUniformLocation(sChar, "uOpaque"), tiles_opaque ? 1.0f : 0.0f);
    glDrawArrays(GL_POINTS, first, count);
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
    glUniform1i(glGetUniformLocation(sAffine, "sTex"), 0);
    glUniform1f(glGetUniformLocation(sAffine, "uRepeat"), over ? 0.0f : 1.0f);
    glUniform1f(glGetUniformLocation(sAffine, "uStartMap"), mapid % AFFINE_CACHE_SIZE);
    glUniform3f(glGetUniformLocation(sAffine, "uWorldSize"), scx, scy, huge_bg ? 8 / scy : scx);
    float vis[8] = {0};
    for (int sub_bg = 0; sub_bg < map_count; sub_bg++) {
        int cache_id = (mapid + sub_bg) % AFFINE_CACHE_SIZE;
        vis[sub_bg] = (visible[sub_bg] && tileMapCache[cache_id].bg == mapid + sub_bg && tileMapCache[cache_id].visible) ? 1.0f : 0.0f;
    }
    glUniform4fv(glGetUniformLocation(sAffine, "uMapVisibleA"), 1, vis);
    glUniform4fv(glGetUniformLocation(sAffine, "uMapVisibleB"), 1, vis + 4);

    for (int eye = 0; eye < 2; eye++) {
        if (world->on & (2 >> eye)) {
            int gx = base_gx + (eye == 0 ? -gp : gp);
            glUniform1f(glGetUniformLocation(sAffine, "uRight"), (float)eye);
            glUniform2f(glGetUniformLocation(sAffine, "uPosOffset"), eye == 0 ? -gp : gp, eye * 256);
            gpu_set_scissor(true, 256 * eye + (gy >= 0 ? gy : 0), gx >= 0 ? gx : 0, (gy + h < 256 ? gy + h : 256) + 256 * eye, gx + w);
            glDrawArrays(GL_LINES, 0, h * 2);
        }
    }
}

void update_texture_cache_hard(void) {
    int min_updated = 2048, max_updated = -1;
    for (int t = 0; t < 2048; t++) {
        if (!tDSPCACHE.CharacterCache[t]) {
            if (blankTile < 0 && !tileVisible[t]) blankTile = t;
            continue;
        }
        if (min_updated > t) min_updated = t;
        if (max_updated < t) max_updated = t;
        u16 *tile = (u16 *)(vb_state->V810_DISPLAY_RAM.off + ((t & 0x600) << 6) + 0x6000 + (t & 0x1ff) * 16);
        int ty = t / 32;
        int tx = t % 32;
        u16 *dstbuf = tileTextureBuffer + ty * 256 * 8 + tx * 8;
        bool tv = ((uint64_t *)tile)[0] | ((uint64_t *)tile)[1];
        tileVisible[t] = tv;
        if (!tv) {
            if (blankTile < 0) blankTile = t;
            for (int y = 0; y < 8; y++) memset(dstbuf + 256 * y, 0, 8 * 2);
            continue;
        }
        static const uint16_t colors[4] = {0, 0xf00f, 0x0f0f, 0x00ff};
        for (int y = 0; y < 8; y++) {
            u16 row = *tile++;
            for (int x = 0; x < 8; x++) {
                *dstbuf++ = colors[row & 3];
                row >>= 2;
            }
            dstbuf += 256 - 8;
        }
    }
    if (min_updated <= max_updated) {
        glBindTexture(GL_TEXTURE_2D, tileTexture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, (min_updated / 32) * 8, 256, (max_updated / 32 + 1 - min_updated / 32) * 8, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, tileTextureBuffer + (min_updated / 32 * 256 * 8));
    }
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
        glReadPixels(eye * 256, 0, DOWNLOADED_FRAMEBUFFER_WIDTH, 384, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, rgba4_framebuffers + (384 * DOWNLOADED_FRAMEBUFFER_WIDTH * eye));
    }
}

void video_download_vip(int drawn_fb) {
    if (tVBOpt.RENDERMODE != RM_TOCPU) return;
    if (downloaded) return;
    downloaded = true;
    for (int eye = 0; eye < 2; eye++) {
        uint32_t *in_fb = (uint32_t *)(rgba4_framebuffers + (384 * DOWNLOADED_FRAMEBUFFER_WIDTH) * eye);
        uint32_t *out_fb = (uint32_t *)(vb_state->V810_DISPLAY_RAM.off + 0x10000 * eye + 0x8000 * drawn_fb);
        for (int x = 0; x < 384; x++) {
            for (int y = 0; y < 224; y += 16) {
                uint32_t buf = 0;
                for (int i = 0; i < 8; i++) {
                    uint32_t inbuf = *in_fb++;
                    buf |= (((inbuf & 0xffff) >> 8) | (inbuf >> 22)) << (i * 4);
                }
                *out_fb++ = buf;
            }
            in_fb += (DOWNLOADED_FRAMEBUFFER_WIDTH - 224) / 2;
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
    glUniform1i(glGetUniformLocation(sFinal, "sVip"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tDSPCACHE.DDSPDataState[displayed_fb] != GPU_CLEAR ? screenTexSoft[displayed_fb] : transparentPixelTexture);
    glUniform1i(glGetUniformLocation(sFinal, "sSoft"), 1);
    glActiveTexture(GL_TEXTURE0);
    glUniform1f(glGetUniformLocation(sFinal, "uVipOverSoft"), tVBOpt.VIP_OVER_SOFT ? 1.0f : 0.0f);
    glUniform3fv(glGetUniformLocation(sFinal, "uPalette"), 4, &pal[0][0]);
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
