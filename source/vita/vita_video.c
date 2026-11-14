#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "v810_mem.h"
#include "vb_dsp.h"
#include "vb_set.h"
#include "video_hard.h"
#include "vita.h"
#include "vita_ui.h"

extern GLuint eyeTex[2];
GLuint vita_build_program(const char *vs, const char *fs, const char *const *attribs, int attrib_count);
bool vita_read_eye(int eye, uint8_t *rgba);
void vita_compose_eyes(void);

static GLuint present_program = 0;
static uint8_t *readback = NULL;
static bool frame_ready = false;

static const char *const present_attribs[] = {"aPos", "aUV"};

void gl_flush(void) {
    frame_ready = true;
}

void video_init_vita(void) {
    present_program = vita_build_program(
        "attribute vec2 aPos;\n"
        "attribute vec2 aUV;\n"
        "uniform vec2 uScale;\n"
        "varying vec2 vUV;\n"
        "void main() {\n"
        "   gl_Position = vec4(aPos.x * uScale.x - 1.0, 1.0 - aPos.y * uScale.y, 0.0, 1.0);\n"
        "   vUV = aUV;\n"
        "}\n",
        "uniform sampler2D sLeft;\n"
        "uniform sampler2D sRight;\n"
        "uniform float uAnaglyph;\n"
        "uniform vec3 uMaskL;\n"
        "uniform vec3 uMaskR;\n"
        "uniform float uGrid;\n"
        "uniform vec2 uTexSize;\n"
        "uniform vec2 uPixel;\n"
        "uniform vec4 uTint;\n"
        "varying vec2 vUV;\n"
        "void main() {\n"
        "   vec3 c = texture2D(sLeft, vUV).rgb;\n"
        "   if (uAnaglyph > 0.5) {\n"
        "       vec3 r = texture2D(sRight, vUV).rgb;\n"
        "       vec3 w = vec3(0.299, 0.587, 0.114);\n"
        "       c = uMaskL * dot(c, w) + uMaskR * dot(r, w);\n"
        "   }\n"
        "   if (uGrid > 0.5) {\n"
        "       vec2 f = fract(vUV * uTexSize);\n"
        "       float g = max(step(1.0 - uPixel.x, f.x), step(1.0 - uPixel.y, f.y));\n"
        "       c *= 1.0 - 0.5 * g;\n"
        "   }\n"
        "   gl_FragColor = vec4(c, 1.0) * uTint;\n"
        "}\n",
        present_attribs, 2);
    readback = malloc(VB_W * VB_H * 4);
    video_apply_filter();
}

void video_shutdown_vita(void) {
    if (present_program) glDeleteProgram(present_program);
    present_program = 0;
    free(readback);
    readback = NULL;
}

void video_apply_filter(void) {
    GLint f = settings.smooth ? GL_LINEAR : GL_NEAREST;
    for (int e = 0; e < 2; e++) {
        if (!eyeTex[e]) continue;
        glBindTexture(GL_TEXTURE_2D, eyeTex[e]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    }
}

void video_clear(void) {
    gpu_clear_screen(false);
    frame_ready = false;
    vita_compose_eyes();
}

void video_recompose(void) {
    vita_compose_eyes();
}

static void single_rect(float *x, float *y, float *w, float *h) {
    float sx = (float)SCREEN_W / VB_W;
    float sy = (float)SCREEN_H / VB_H;
    switch (settings.scale_mode) {
        case SCALE_INTEGER: {
            float s = floorf(sx < sy ? sx : sy);
            *w = VB_W * s;
            *h = VB_H * s;
            break;
        }
        case SCALE_STRETCH:
            *w = SCREEN_W;
            *h = SCREEN_H;
            break;
        default: {
            float s = sx < sy ? sx : sy;
            *w = VB_W * s;
            *h = VB_H * s;
            break;
        }
    }
    *x = (SCREEN_W - *w) / 2;
    *y = (SCREEN_H - *h) / 2;
}

void video_game_rect(float *x, float *y, float *w, float *h) {
    if (settings.stereo == STEREO_SIDE_BY_SIDE) {
        *w = SCREEN_W / 2;
        *h = VB_H * ((float)(SCREEN_W / 2) / VB_W);
        *x = 0;
        *y = (SCREEN_H - *h) / 2;
        return;
    }
    single_rect(x, y, w, h);
}

static void draw_eye_quad(GLuint left, GLuint right, bool anaglyph, float x, float y, float w, float h, uint32_t tint, bool grid) {
    ui_flush();
    glUseProgram(present_program);
    glUniform2f(glGetUniformLocation(present_program, "uScale"), 2.0f / SCREEN_W, 2.0f / SCREEN_H);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, right);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, left);
    glUniform1i(glGetUniformLocation(present_program, "sLeft"), 0);
    glUniform1i(glGetUniformLocation(present_program, "sRight"), 1);
    glUniform1f(glGetUniformLocation(present_program, "uAnaglyph"), anaglyph ? 1.0f : 0.0f);
    float mr[3] = {0, 1, 1};
    if (settings.anaglyph == ANAGLYPH_RED_BLUE) mr[1] = 0;
    if (settings.anaglyph == ANAGLYPH_RED_GREEN) mr[2] = 0;
    glUniform3f(glGetUniformLocation(present_program, "uMaskL"), 1, 0, 0);
    glUniform3fv(glGetUniformLocation(present_program, "uMaskR"), 1, mr);
    glUniform1f(glGetUniformLocation(present_program, "uGrid"), grid ? 1.0f : 0.0f);
    glUniform2f(glGetUniformLocation(present_program, "uTexSize"), VB_W, VB_H);
    glUniform2f(glGetUniformLocation(present_program, "uPixel"), w > 0 ? VB_W / w : 1.0f, h > 0 ? VB_H / h : 1.0f);
    glUniform4f(glGetUniformLocation(present_program, "uTint"), (tint & 0xFF) / 255.0f, ((tint >> 8) & 0xFF) / 255.0f, ((tint >> 16) & 0xFF) / 255.0f, ((tint >> 24) & 0xFF) / 255.0f);
    GLfloat pos[8] = {x, y, x + w, y, x, y + h, x + w, y + h};
    static const GLfloat uv[8] = {0, 1, 1, 1, 0, 0, 1, 0};
    glDisableVertexAttribArray(2);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    ui_restore_state();
}

static void draw_game(float x, float y, float w, float h, uint32_t tint, bool grid) {
    switch (settings.stereo) {
        case STEREO_SIDE_BY_SIDE: {
            float half = w / 2;
            float hh = VB_H * (half / VB_W);
            float yy = y + (h - hh) / 2;
            draw_eye_quad(eyeTex[0], eyeTex[0], false, x, yy, half, hh, tint, grid);
            draw_eye_quad(eyeTex[1], eyeTex[1], false, x + half, yy, half, hh, tint, grid);
            break;
        }
        case STEREO_ANAGLYPH:
            draw_eye_quad(eyeTex[0], eyeTex[1], true, x, y, w, h, tint, grid);
            break;
        case STEREO_RIGHT:
            draw_eye_quad(eyeTex[1], eyeTex[1], false, x, y, w, h, tint, grid);
            break;
        default:
            draw_eye_quad(eyeTex[0], eyeTex[0], false, x, y, w, h, tint, grid);
            break;
    }
}

void video_present_game(float dim) {
    uint8_t v = (uint8_t)(255.0f * (1.0f - dim));
    float x, y, w, h;
    if (settings.stereo == STEREO_SIDE_BY_SIDE) {
        video_game_rect(&x, &y, &w, &h);
        x = 0;
        w = SCREEN_W;
        h = SCREEN_H;
        y = 0;
    } else {
        video_game_rect(&x, &y, &w, &h);
    }
    bool grid = settings.led_grid && (settings.stereo == STEREO_SIDE_BY_SIDE ? (SCREEN_W / 2.0f) / VB_W : w / VB_W) >= 1.9f;
    draw_game(x, y, w, h, RGBA8(v, v, v, 255), grid);
    frame_ready = false;
}

void video_draw_game_at(float x, float y, float w, float h, uint32_t tint) {
    draw_game(x, y, w, h, tint, false);
}

bool video_make_thumbnail(uint32_t *dst, int w, int h) {
    int eye = settings.stereo == STEREO_RIGHT ? 1 : 0;
    if (!readback || !vita_read_eye(eye, readback)) return false;
    ui_restore_state();
    const uint32_t *src = (const uint32_t *)readback;
    for (int y = 0; y < h; y++) {
        int sy0 = y * VB_H / h;
        int sy1 = (y + 1) * VB_H / h;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < w; x++) {
            int sx0 = x * VB_W / w;
            int sx1 = (x + 1) * VB_W / w;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int yy = sy0; yy < sy1; yy++) {
                const uint32_t *row = src + (VB_H - 1 - yy) * VB_W;
                for (int xx = sx0; xx < sx1; xx++) {
                    uint32_t c = row[xx];
                    r += c & 0xFF;
                    g += (c >> 8) & 0xFF;
                    b += (c >> 16) & 0xFF;
                    n++;
                }
            }
            dst[y * w + x] = 0xFF000000 | ((b / n) << 16) | ((g / n) << 8) | (r / n);
        }
    }
    return true;
}
