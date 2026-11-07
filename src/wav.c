/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "wav.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint16_t get16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

struct WavOut { FILE *f; int bits, rate, ok; uint32_t frames; char path[1024], tmp[1100]; };

static void header(uint8_t *h, int bits, int rate, uint32_t frames)
{
    uint32_t bps = bits / 8, data = frames * 2 * bps;
    memcpy(h, "RIFF", 4); put32(h + 4, 36 + data); memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16); put16(h + 20, bits == 32 ? 3 : 1); put16(h + 22, 2);
    put32(h + 24, rate); put32(h + 28, rate * 2 * bps); put16(h + 32, 2 * bps);
    put16(h + 34, bits); memcpy(h + 36, "data", 4); put32(h + 40, data);
}

WavOut *wav_open(const char *path, int rate, int bits)
{
    WavOut *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->bits = bits == 32 ? 32 : 16; w->rate = rate;
    snprintf(w->path, sizeof w->path, "%s", path);
    snprintf(w->tmp, sizeof w->tmp, "%s.part", path);
    w->f = fopen(w->tmp, "wb");
    if (!w->f) { free(w); return NULL; }
    uint8_t h[44]; header(h, w->bits, rate, 0);
    w->ok = fwrite(h, 1, 44, w->f) == 44;
    return w;
}

int wav_append(WavOut *w, const float *st, int frames)
{
    uint32_t bps = w->bits / 8;
    uint8_t chunk[8192];
    int per = (int)(sizeof chunk / (2 * bps));
    for (int i = 0; w->ok && i < frames; i += per) {
        int n = frames - i < per ? frames - i : per;
        for (int j = 0; j < n * 2; j++) {
            float v = st[(size_t)i * 2 + j];
            if (v > 1.f) v = 1.f; else if (v < -1.f) v = -1.f;
            if (w->bits == 32) memcpy(chunk + j * 4, &v, 4);  /* little-endian targets */
            else put16(chunk + j * 2, (uint16_t)(int16_t)(v * 32767.f + (v >= 0 ? 0.5f : -0.5f)));
        }
        w->ok = fwrite(chunk, 1, (size_t)n * 2 * bps, w->f) == (size_t)n * 2 * bps;
    }
    if (w->ok) w->frames += (uint32_t)frames;
    return w->ok ? 0 : -1;
}

int wav_close(WavOut *w, int keep)
{
    int ok = w->ok && keep;
    if (ok) {
        uint8_t h[44]; header(h, w->bits, w->rate, w->frames);
        ok = fseek(w->f, 0, SEEK_SET) == 0 && fwrite(h, 1, 44, w->f) == 44;
    }
    ok = ok && fflush(w->f) == 0 && fsync(fileno(w->f)) == 0;
    ok = (fclose(w->f) == 0) && ok;
    if (!ok || rename(w->tmp, w->path) != 0) { remove(w->tmp); ok = 0; }
    free(w);
    return ok ? 0 : -1;
}

int wav_write(const char *path, const float *st, int frames, int rate, int bits)
{
    WavOut *w = wav_open(path, rate, bits);
    if (!w) return -1;
    wav_append(w, st, frames);
    int r = wav_close(w, 1);
    if (!r) sync();
    return r;
}

float *wav_read(const char *path, int dst_rate, int max_frames, int *out_frames)
{
    *out_frames = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { fclose(f); return NULL; }
    int fmt = 0, ch = 0, rate = 0, bits = 0;
    uint8_t *raw = NULL; uint32_t rawlen = 0;
    uint8_t ck[8];
    while (fread(ck, 1, 8, f) == 8) {
        uint32_t sz = get32(ck + 4);
        if (!memcmp(ck, "fmt ", 4) && sz >= 16 && sz < 256) {
            uint8_t b[256];
            if (fread(b, 1, sz, f) != sz) break;
            fmt = get16(b); ch = get16(b + 2); rate = (int)get32(b + 4); bits = get16(b + 14);
            if (fmt == 0xFFFE && sz >= 26) fmt = get16(b + 24);
            if (sz & 1) fgetc(f);
        } else if (!memcmp(ck, "data", 4)) {
            long cur = ftell(f); fseek(f, 0, SEEK_END); long end = ftell(f); fseek(f, cur, SEEK_SET);
            if (sz == 0 || sz > (uint32_t)(end - cur)) sz = (uint32_t)(end - cur);  /* streamed/truncated files */
            raw = malloc(sz ? sz : 1);
            if (!raw) break;
            rawlen = (uint32_t)fread(raw, 1, sz, f);
            break;
        } else {
            if (fseek(f, sz + (sz & 1), SEEK_CUR)) break;
        }
    }
    fclose(f);
    int bps = bits / 8;
    if (!raw || ch < 1 || rate < 1000 || bps < 1 || !(fmt == 1 || fmt == 3) ||
        (fmt == 3 && bits != 32 && bits != 64)) { free(raw); return NULL; }
    long n = rawlen / (uint32_t)(ch * bps);
    if (n < 1) { free(raw); return NULL; }

    /* decode to stereo float at source rate */
    float *src = malloc((size_t)n * 2 * sizeof(float));
    if (!src) { free(raw); return NULL; }
    for (long i = 0; i < n; i++)
        for (int c = 0; c < 2; c++) {
            const uint8_t *p = raw + (size_t)(i * ch + (c < ch ? c : 0)) * bps;
            float v;
            if (fmt == 3 && bits == 32) memcpy(&v, p, 4);
            else if (fmt == 3) { double d; memcpy(&d, p, 8); v = (float)d; }
            else if (bits == 8) v = (p[0] - 128) / 128.f;
            else if (bits == 16) v = (int16_t)get16(p) / 32768.f;
            else if (bits == 24) v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) / 2147483648.f;
            else v = (int32_t)get32(p) / 2147483648.f;
            src[2 * i + c] = v;
        }
    free(raw);
    if (rate == dst_rate) {
        if (n > max_frames) n = max_frames;
        *out_frames = (int)n;
        return src;
    }
    /* linear resample */
    double ratio = (double)rate / dst_rate;
    long m = (long)(n / ratio);
    if (m > max_frames) m = max_frames;
    float *dst = malloc((size_t)(m > 0 ? m : 1) * 2 * sizeof(float));
    if (!dst) { free(src); return NULL; }
    for (long i = 0; i < m; i++) {
        double sp = i * ratio; long a = (long)sp; float fr = (float)(sp - a);
        long b = a + 1 < n ? a + 1 : a;
        for (int c = 0; c < 2; c++) dst[2 * i + c] = src[2 * a + c] * (1 - fr) + src[2 * b + c] * fr;
    }
    free(src);
    *out_frames = (int)m;
    return dst;
}
