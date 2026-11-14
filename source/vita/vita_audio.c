#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <string.h>

#include "vb_set.h"
#include "vb_sound.h"
#include "vita.h"

#define CORE_RATE 50000
#define CORE_CHUNK (CORE_RATE / 100)
#define OUT_RATE 48000
#define OUT_GRAIN 256
#define RING_FRAMES 16384
#define RING_MASK (RING_FRAMES - 1)
#define TARGET_FILL 3000

static int16_t ring[RING_FRAMES * 2];
static volatile uint32_t ring_head = 0;
static volatile uint32_t ring_tail = 0;
static volatile bool audio_running = false;
static volatile bool audio_paused = true;
static SceUID audio_thread = -1;
static int audio_port = -1;
static int16_t out_buf[2][OUT_GRAIN * 2];
static int applied_volume = -1;

static inline uint32_t ring_count(void) {
    return ring_head - ring_tail;
}

static void apply_volume(void) {
    if (audio_port < 0) return;
    int v = settings.volume;
    if (v < 0) v = 0;
    if (v > 10) v = 10;
    int vol = SCE_AUDIO_VOLUME_0DB * v / 10;
    int vols[2] = {vol, vol};
    sceAudioOutSetVolume(audio_port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vols);
    applied_volume = settings.volume;
}

static int audio_thread_main(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    uint32_t frac = 0;
    int which = 0;
    while (audio_running) {
        if (applied_volume != settings.volume) apply_volume();
        int16_t *out = out_buf[which];
        which ^= 1;
        uint32_t avail = ring_count();
        if (audio_paused || !settings.sound || avail < OUT_GRAIN * 2) {
            memset(out, 0, sizeof(out_buf[0]));
            if (audio_paused || !settings.sound) frac = 0;
            sceAudioOutOutput(audio_port, out);
            continue;
        }
        int32_t drift = (int32_t)avail - TARGET_FILL;
        if (drift > 2000) drift = 2000;
        if (drift < -2000) drift = -2000;
        uint32_t step = (uint32_t)(((uint64_t)CORE_RATE << 16) / OUT_RATE);
        step += (uint32_t)((int32_t)step * drift / 200000);
        uint32_t tail = ring_tail;
        for (int i = 0; i < OUT_GRAIN; i++) {
            uint32_t idx = tail & RING_MASK;
            uint32_t nidx = (tail + 1) & RING_MASK;
            int32_t t = frac >> 4;
            int32_t l0 = ring[idx * 2], r0 = ring[idx * 2 + 1];
            int32_t l1 = ring[nidx * 2], r1 = ring[nidx * 2 + 1];
            out[i * 2] = (int16_t)(l0 + (((l1 - l0) * t) >> 12));
            out[i * 2 + 1] = (int16_t)(r0 + (((r1 - r0) * t) >> 12));
            frac += step;
            tail += frac >> 16;
            frac &= 0xFFFF;
        }
        __sync_synchronize();
        ring_tail = tail;
        sceAudioOutOutput(audio_port, out);
    }
    return 0;
}

bool audio_init(void) {
    audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, OUT_GRAIN, OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (audio_port < 0) return false;
    apply_volume();
    audio_running = true;
    audio_thread = sceKernelCreateThread("rv_audio", audio_thread_main, 0x10000100, 0x10000, 0, 0, NULL);
    if (audio_thread < 0) {
        audio_running = false;
        sceAudioOutReleasePort(audio_port);
        audio_port = -1;
        return false;
    }
    sceKernelStartThread(audio_thread, 0, NULL);
    return true;
}

void audio_shutdown(void) {
    if (audio_thread >= 0) {
        audio_running = false;
        sceKernelWaitThreadEnd(audio_thread, NULL, NULL);
        sceKernelDeleteThread(audio_thread);
        audio_thread = -1;
    }
    if (audio_port >= 0) {
        sceAudioOutReleasePort(audio_port);
        audio_port = -1;
    }
}

void audio_clear(void) {
    ring_tail = ring_head;
}

int audio_fill_percent(void) {
    return (int)(ring_count() * 100 / RING_FRAMES);
}

bool sound_init_backend(int16_t *wavebufs[]) {
    (void)wavebufs;
    return true;
}

void sound_close_backend(void) {
    audio_paused = true;
}

void sound_pause_backend(void) {
    audio_paused = true;
}

void sound_resume_backend(void) {
    audio_clear();
    audio_paused = false;
}

bool sound_push_backend(int16_t *buf) {
    uint32_t head = ring_head;
    if (head - ring_tail + CORE_CHUNK >= RING_FRAMES - 1) return false;
    if (tVBOpt.FASTFORWARD && head - ring_tail > TARGET_FILL * 2) return false;
    for (int i = 0; i < CORE_CHUNK; i++) {
        uint32_t idx = (head + i) & RING_MASK;
        ring[idx * 2] = buf[i * 2];
        ring[idx * 2 + 1] = buf[i * 2 + 1];
    }
    __sync_synchronize();
    ring_head = head + CORE_CHUNK;
    return true;
}
