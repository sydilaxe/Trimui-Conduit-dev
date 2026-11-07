/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "midi.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int midi_parse(MidiParser *p, uint8_t b, MidiMsg *m)
{
    if (b >= 0xF8) { memset(m, 0, sizeof *m); m->type = MM_RT; m->num = b; return 1; }
    if (b & 0x80) {
        p->sysex = b == 0xF0;
        if (b >= 0xF0) {
            /* system common: skip its data (0xF1, 0xF3: one byte; 0xF2: two) */
            p->status = 0; p->n = 0;
            p->need = b == 0xF2 ? 2 : (b == 0xF1 || b == 0xF3) ? 1 : 0;
            p->d0 = 0;
            return 0;
        }
        p->status = b; p->n = 0;
        p->need = (b & 0xF0) == 0xC0 || (b & 0xF0) == 0xD0 ? 1 : 2;
        return 0;
    }
    if (p->sysex) return 0;
    if (!p->status) { if (p->need && --p->need == 0) {} return 0; }
    if (p->n == 0) { p->d0 = b; p->n = 1; if (p->need == 2) return 0; }
    else p->n = 2;
    /* complete (running status keeps p->status for the next one) */
    uint8_t hi = p->status & 0xF0;
    memset(m, 0, sizeof *m);
    m->ch = p->status & 0x0F;
    p->n = 0;
    switch (hi) {
    case 0x90: m->type = b ? MM_NOTE : MM_NOTE_OFF; m->num = p->d0; m->val = b; return 1;
    case 0x80: m->type = MM_NOTE_OFF; m->num = p->d0; m->val = b; return 1;
    case 0xB0: m->type = MM_CC; m->num = p->d0; m->val = b; return 1;
    case 0xE0: m->type = MM_PB; m->val = (uint16_t)(p->d0 | b << 7); return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ reader */

#define MAXDEV 8
#define UIQ 256
static struct {
    Engine *e;
    pthread_t th; int started; volatile int quit;
    int fd[MAXDEV]; char path[MAXDEV][300], name[MAXDEV][64]; MidiParser ps[MAXDEV];
    MidiMsg q[UIQ]; volatile uint32_t qw, qr;
    pthread_mutex_t name_mx;
    char names[256];
} M = { .name_mx = PTHREAD_MUTEX_INITIALIZER };

static void card_name(const char *path, char *out, size_t sz)
{
    int card = -1, dev = 0;
    const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
    if (sscanf(b, "midiC%dD%d", &card, &dev) == 2) {
        char p[64]; snprintf(p, sizeof p, "/proc/asound/card%d/id", card);
        FILE *f = fopen(p, "r");
        if (f) {
            char id[48] = { 0 };
            if (fgets(id, sizeof id, f)) { id[strcspn(id, "\r\n")] = 0; snprintf(out, sz, "%s%s", id, dev ? " 2" : ""); fclose(f); return; }
            fclose(f);
        }
    }
    snprintf(out, sz, "%s", b);
}

static int is_open(const char *path)
{
    for (int i = 0; i < MAXDEV; i++) if (M.fd[i] >= 0 && !strcmp(M.path[i], path)) return 1;
    return 0;
}

static void try_open(const char *path)
{
    if (is_open(path)) return;
    for (int i = 0; i < MAXDEV; i++)
        if (M.fd[i] < 0) {
            int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) return;
            M.fd[i] = fd;
            snprintf(M.path[i], sizeof M.path[i], "%s", path);
            card_name(path, M.name[i], sizeof M.name[i]);
            memset(&M.ps[i], 0, sizeof M.ps[i]);
            fprintf(stderr, "[conduit] MIDI in: %s (%s)\n", M.name[i], path);
            return;
        }
}

static void update_names(void)
{
    char b[256] = { 0 };
    for (int i = 0; i < MAXDEV; i++)
        if (M.fd[i] >= 0) {
            size_t l = strlen(b);
            snprintf(b + l, sizeof b - l, "%s%s", l ? ", " : "", M.name[i]);
        }
    pthread_mutex_lock(&M.name_mx);
    memcpy(M.names, b, sizeof b);
    pthread_mutex_unlock(&M.name_mx);
}

static void rescan(void)
{
    const char *env = getenv("CONDUIT_MIDI_DEV");
    if (env && *env) {
        char list[512]; snprintf(list, sizeof list, "%s", env);
        for (char *s = strtok(list, ":"); s; s = strtok(NULL, ":")) try_open(s);
    } else {
        DIR *d = opendir("/dev/snd");
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)))
                if (!strncmp(de->d_name, "midiC", 5)) {
                    char p[300]; snprintf(p, sizeof p, "/dev/snd/%.250s", de->d_name);
                    try_open(p);
                }
            closedir(d);
        }
    }
    update_names();
}

static void close_dev(int i)
{
    fprintf(stderr, "[conduit] MIDI in closed: %s\n", M.name[i]);
    close(M.fd[i]); M.fd[i] = -1;
    update_names();
}

static void *reader(void *arg)
{
    (void)arg;
    double next_scan = 0;
    while (!M.quit) {
        double now = eng_clock();
        if (now >= next_scan) { rescan(); next_scan = now + 2.0; }
        struct pollfd pf[MAXDEV]; int map[MAXDEV], n = 0;
        for (int i = 0; i < MAXDEV; i++) if (M.fd[i] >= 0) { pf[n].fd = M.fd[i]; pf[n].events = POLLIN; pf[n].revents = 0; map[n++] = i; }
        if (!n) { usleep(100000); continue; }
        int r = poll(pf, n, 200);
        if (r <= 0) continue;
        for (int k = 0; k < n; k++) {
            if (!pf[k].revents) continue;
            int i = map[k];
            uint8_t buf[256];
            ssize_t got = read(M.fd[i], buf, sizeof buf);
            double t = eng_clock();
            if (got <= 0) {
                if (got < 0 && (errno == EAGAIN || errno == EINTR)) continue;
                close_dev(i);   /* unplugged (or the test's writer is done) */
                continue;
            }
            for (ssize_t j = 0; j < got; j++) {
                MidiMsg m;
                if (!midi_parse(&M.ps[i], buf[j], &m)) continue;
                if (m.type == MM_RT) {
                    if (m.num == 0xF8 || m.num == 0xFA || m.num == 0xFB || m.num == 0xFC) eng_midi_clock(M.e, m.num, t);
                    continue;
                }
                uint32_t w = M.qw;
                if (w - __atomic_load_n(&M.qr, __ATOMIC_ACQUIRE) < UIQ) {
                    M.q[w & (UIQ - 1)] = m;
                    __atomic_store_n(&M.qw, w + 1, __ATOMIC_RELEASE);
                }
            }
        }
    }
    return NULL;
}

int midi_start(Engine *e)
{
    if (M.started) return 0;
    M.e = e;
    for (int i = 0; i < MAXDEV; i++) M.fd[i] = -1;
    M.quit = 0;
    if (pthread_create(&M.th, NULL, reader, NULL)) return -1;
    M.started = 1;
    return 0;
}

void midi_stop(void)
{
    if (!M.started) return;
    M.quit = 1;
    pthread_join(M.th, NULL);
    for (int i = 0; i < MAXDEV; i++) if (M.fd[i] >= 0) { close(M.fd[i]); M.fd[i] = -1; }
    M.started = 0;
}

int midi_poll(MidiMsg *m)
{
    uint32_t r = M.qr;
    if (r == __atomic_load_n(&M.qw, __ATOMIC_ACQUIRE)) return 0;
    *m = M.q[r & (UIQ - 1)];
    __atomic_store_n(&M.qr, r + 1, __ATOMIC_RELEASE);
    return 1;
}

void midi_devices(char *out, int sz)
{
    pthread_mutex_lock(&M.name_mx);
    snprintf(out, sz, "%s", M.names);
    pthread_mutex_unlock(&M.name_mx);
}
