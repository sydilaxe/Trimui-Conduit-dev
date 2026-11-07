/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "midi.h"
#include "link.h"
#include <linux/usbdevice_fs.h>
#include <math.h>
#include <sys/ioctl.h>
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
    if (b == 0xF7 && p->sysex) p->sx_done = 1;   /* the caller looks at p->sx */
    if (b == 0xF0) p->sxn = 0;
    if (p->sysex && !(b & 0x80)) { if (p->sxn < (int)sizeof p->sx) p->sx[p->sxn++] = b; else p->sxn = (int)sizeof p->sx + 1; return 0; }
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
    int fd[MAXDEV], wr[MAXDEV]; char path[MAXDEV][300], name[MAXDEV][64]; MidiParser ps[MAXDEV];
    /* Bricks following on the USB link: their usbfs devices and bulk OUT endpoints */
    int lfd[MAXDEV], lep[MAXDEV], ldead[MAXDEV], nl; char lpath[MAXDEV][300];
    MidiParser lps;   /* what a leading Brick sends this one */
    MidiClockOut co; double link_sent; int send_clock;
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
            /* read and write (the clock out); a test's FIFO only read, so it ends when its writer does */
            int test = getenv("CONDUIT_MIDI_DEV") && *getenv("CONDUIT_MIDI_DEV");
            int fd = test ? -1 : open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
            M.wr[i] = fd >= 0;
            if (fd < 0) fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
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

/* ------------------------------------------------- the clock in, and the link */

static pthread_mutex_t clk_mx = PTHREAD_MUTEX_INITIALIZER; /* the reader and the link's thread both feed the engine */

static void dispatch(MidiParser *ps, const uint8_t *buf, int got, double t, int to_ui)
{
    for (int j = 0; j < got; j++) {
        MidiMsg m;
        int msg = midi_parse(ps, buf[j], &m);
        if (ps->sx_done) {
            ps->sx_done = 0;
            const uint8_t *x = ps->sx;
            if (ps->sxn == 12 && x[0] == 0x7D && x[1] == 'T' && x[2] == 'L') {
                uint64_t pp = 0; uint32_t bb = 0;
                for (int k = 0; k < 5; k++) pp |= (uint64_t)x[3 + k] << (7 * k);
                for (int k = 0; k < 3; k++) bb |= (uint32_t)x[8 + k] << (7 * k);
                pthread_mutex_lock(&clk_mx);
                eng_link_pos(M.e, pp / 65536.0, bb / 1000.0, x[11] & 1, t);
                pthread_mutex_unlock(&clk_mx);
            }
        }
        if (!msg) continue;
        if (m.type == MM_RT) {
            if (m.num == 0xF8 || m.num == 0xFA || m.num == 0xFB || m.num == 0xFC) {
                pthread_mutex_lock(&clk_mx);
                eng_midi_clock(M.e, m.num, t);
                pthread_mutex_unlock(&clk_mx);
            }
            continue;
        }
        if (!to_ui) continue;
        uint32_t w = M.qw;
        if (w - __atomic_load_n(&M.qr, __ATOMIC_ACQUIRE) < UIQ) {
            M.q[w & (UIQ - 1)] = m;
            __atomic_store_n(&M.qw, w + 1, __ATOMIC_RELEASE);
        }
    }
}

/* the link's thread: what a leading Brick sends */
static void link_bytes(const uint8_t *b, int n, double t) { dispatch(&M.lps, b, n, t, 0); }

int midi_link_pos(uint8_t *out, int max, double pos16, double bpm, int playing)
{
    if (max < 14) return 0;
    uint64_t p = pos16 > 0.0 ? (uint64_t)llround(pos16 * 65536.0) : 0;
    uint32_t b = bpm > 0.0 ? (uint32_t)lround(bpm * 1000.0) : 0;
    int n = 0;
    out[n++] = 0xF0, out[n++] = 0x7D, out[n++] = 'T', out[n++] = 'L';
    for (int k = 0; k < 5; k++) out[n++] = (uint8_t)(p >> (7 * k) & 127);
    for (int k = 0; k < 3; k++) out[n++] = (uint8_t)(b >> (7 * k) & 127);
    out[n++] = playing ? 1 : 0;
    out[n++] = 0xF7;
    return n;
}

/* 24 ticks a beat (6 a 16th): tick k goes when the sound heard reaches k/6 16ths.
 * Started on the first 16th: Start, and the next tick is the first. Started
 * later: song position (the next 16th) and Continue as the sound gets there. */
int midi_clock_out(MidiClockOut *co, double pos16, int run, uint8_t *out, int max)
{
    int n = 0;
    if (!run) {
        if (co->running && n < max) out[n++] = 0xFC;
        co->running = co->pending = 0;
        return n;
    }
    long want = (long)floor(pos16 * 6.0);
    if (!co->running) {
        co->running = 1;
        if (pos16 < 1.0 / 6.0) {
            out[n++] = 0xFA;
            co->sent = want;
        } else if (max >= 3) {
            long spp = (long)ceil(pos16);
            out[n++] = 0xF2, out[n++] = (uint8_t)(spp & 127), out[n++] = (uint8_t)(spp >> 7 & 127);
            co->cont_at = spp * 6;
            co->pending = 1;
        }
        return n;
    }
    if (co->pending) {
        if (want < co->cont_at) return 0;
        out[n++] = 0xFB;
        co->pending = 0;
        co->sent = co->cont_at;
    }
    if (want < co->sent - 6) co->sent = want;   /* moved back: carry on from there */
    while (co->sent < want && n < max) { out[n++] = 0xF8; co->sent++; }
    return n;
}

static void send_ports(const uint8_t *b, int n, int links_only)
{
    if (!links_only)
        for (int i = 0; i < MAXDEV; i++)
            if (M.fd[i] >= 0 && M.wr[i] && write(M.fd[i], b, (size_t)n) < 0) { /* full or gone: dropped when read */ }
    for (int i = 0; i < M.nl; i++) {
        if (M.ldead[i]) continue;
        struct usbdevfs_bulktransfer bt = { .ep = (unsigned)M.lep[i], .len = (unsigned)n, .timeout = 20, .data = (void *)b };
        if (ioctl(M.lfd[i], USBDEVFS_BULK, &bt) < 0 && (errno == ENODEV || errno == ESHUTDOWN)) M.ldead[i] = 1;
    }
}

/* Bricks following on the USB link: new ones opened, gone ones dropped */
static void scan_links(void)
{
    LinkDev ld[MAXDEV];
    int n = link_find("/sys/bus/usb/devices", ld, MAXDEV);
    for (int i = M.nl - 1; i >= 0; i--) {
        int still = 0;
        for (int k = 0; k < n && !M.ldead[i]; k++) still |= !strcmp(ld[k].path, M.lpath[i]);
        if (still) continue;
        fprintf(stderr, "[conduit] link: a following Brick went\n");
        close(M.lfd[i]);
        M.nl--;
        M.lfd[i] = M.lfd[M.nl], M.lep[i] = M.lep[M.nl], M.ldead[i] = M.ldead[M.nl];
        memcpy(M.lpath[i], M.lpath[M.nl], sizeof M.lpath[i]);
    }
    for (int k = 0; k < n && M.nl < MAXDEV; k++) {
        int known = 0;
        for (int i = 0; i < M.nl; i++) known |= !strcmp(M.lpath[i], ld[k].path);
        if (known) continue;
        char dev[64]; snprintf(dev, sizeof dev, "/dev/bus/usb/%03d/%03d", ld[k].bus, ld[k].dev);
        int fd = open(dev, O_RDWR | O_CLOEXEC);
        unsigned iface = 0;
        if (fd < 0 || ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface) < 0) { if (fd >= 0) close(fd); continue; }
        M.lfd[M.nl] = fd, M.lep[M.nl] = ld[k].ep, M.ldead[M.nl] = 0;
        snprintf(M.lpath[M.nl], sizeof M.lpath[0], "%s", ld[k].path);
        M.nl++;
        fprintf(stderr, "[conduit] link: a Brick follows this one (%s)\n", dev);
    }
}

void midi_link_status(int *followers, int *follow_state)
{
    *followers = M.nl;
    *follow_state = link_follow_state();
}

/* the clock out and the link's positions, now */
static int clock_step(double now)
{
    double pos = 0, bpm = 0; int run = 0, fol = 0;
    int have = eng_heard(M.e, now, &pos, &bpm, &run, &fol);
    int lead = have && M.send_clock && M.e->p[P_CLOCK_OUT] > 0.5f && !fol;
    uint8_t out[32];
    int n = midi_clock_out(&M.co, pos, lead && run, out, sizeof out);
    if (n) send_ports(out, n, 0);
    /* to the Bricks following: where the sound is, leading or passing another clock on */
    if (M.nl && have && (lead || fol) && now - M.link_sent >= 0.02) {
        M.link_sent = now;
        n = midi_link_pos(out, sizeof out, pos, bpm, run);
        send_ports(out, n, 1);
    }
    return lead && run;
}

static void *reader(void *arg)
{
    (void)arg;
    double next_scan = 0, next_links = 0;
    while (!M.quit) {
        double now = eng_clock();
        if (now >= next_scan) { rescan(); next_scan = now + 2.0; }
        if (now >= next_links) { scan_links(); next_links = now + 1.0; }
        /* the clock out wants the thread every millisecond while it runs */
        int busy = clock_step(now) || M.nl;
        struct pollfd pf[MAXDEV]; int map[MAXDEV], n = 0;
        for (int i = 0; i < MAXDEV; i++) if (M.fd[i] >= 0) { pf[n].fd = M.fd[i]; pf[n].events = POLLIN; pf[n].revents = 0; map[n++] = i; }
        if (!n) { usleep(busy ? 1000 : 100000); continue; }
        int r = poll(pf, n, busy ? 1 : 200);
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
            dispatch(&M.ps[i], buf, (int)got, t, 1);
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
    const char *v = getenv("CONDUIT_MIDI_CLOCK");
    M.send_clock = !(v && *v == '0');
    memset(&M.co, 0, sizeof M.co);
    memset(&M.lps, 0, sizeof M.lps);
    if (pthread_create(&M.th, NULL, reader, NULL)) return -1;
    M.started = 1;
    /* the bottom port as a clock link: a Brick with a hub can lead this one */
    link_follow_start(link_bytes);
    return 0;
}

void midi_stop(void)
{
    if (!M.started) return;
    link_follow_stop();
    M.quit = 1;
    pthread_join(M.th, NULL);
    uint8_t stop = 0xFC;
    if (M.co.running) send_ports(&stop, 1, 0);
    for (int i = 0; i < MAXDEV; i++) if (M.fd[i] >= 0) { close(M.fd[i]); M.fd[i] = -1; }
    for (int i = 0; i < M.nl; i++) close(M.lfd[i]);
    M.nl = 0;
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
