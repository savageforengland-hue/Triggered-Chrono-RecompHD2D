/* SPDX-FileCopyrightText: 2026 jerbanpls
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Save states (savestate.h). */
#include "savestate.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bus.h"
#include "module.h"
#include "sched.h"
#include "sha1.h"

static void put32(uint8_t *b, uint32_t v)
{
    for (int k = 0; k < 4; k++)
        b[k] = (uint8_t)(v >> (8 * k));
}

static uint32_t get32(const uint8_t *b)
{
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

/* ---- header ---- */

#define HEAD_LEN (8 + 4 + 4 + 20 + 8)

const char *savestate_rom_sha1(void)
{
    static char hex[41];
    if (!hex[0])
        sha1_hex(bus_rom(), CT_ROM_SIZE, hex);
    return hex;
}

static void rom_sha1_bin(uint8_t out[20])
{
    const char *h = savestate_rom_sha1();
    for (int k = 0; k < 20; k++) {
        unsigned v;
        sscanf(h + 2 * k, "%2x", &v);
        out[k] = (uint8_t)v;
    }
}

/* The parts, in order. */
static void walk(ss_io *s)
{
    bus_state(s);
    snes_hw_state(s);
    spc_host_state(s);
    interp_state(s);
    sched_state(s);
}

int savestate_ready(const char **why)
{
    return sched_state_ready(why);
}

/* port: the oldest module ABI whose states this build still loads. ABI 9
   added music/text exports, ABI 10 a scene-mode meaning and ABI 11
   per-frame scene flags; none changes what a state holds, so states saved
   at ABI 7 onward still load; a section whose layout did change is refused
   by its length anyway. */
#define SAVESTATE_ABI_MIN 7

uint8_t *savestate_save(size_t *len, const char **why)
{
    if (savestate_ready(why))
        return NULL;
    ss_io s = {0};
    s.version = SAVESTATE_VERSION;
    uint8_t head[HEAD_LEN];
    memcpy(head, SAVESTATE_MAGIC, 8);
    put32(head + 8, SAVESTATE_VERSION);
    put32(head + 12, CT_MODULE_ABI);
    rom_sha1_bin(head + 16);
    uint64_t f = (uint64_t)sched_frame_count();
    for (int k = 0; k < 8; k++)
        head[36 + k] = (uint8_t)(f >> (8 * k));
    ss_bytes(&s, head, sizeof head);
    walk(&s);
    if (s.err) {
        free(s.buf);
        if (why)
            *why = s.err;
        return NULL;
    }
    *len = s.len;
    return s.buf;
}

int savestate_peek(const uint8_t *blob, size_t len, long *frame)
{
    if (len < HEAD_LEN || memcmp(blob, SAVESTATE_MAGIC, 8))
        return -1;
    uint64_t f = 0;
    for (int k = 0; k < 8; k++)
        f |= (uint64_t)blob[36 + k] << (8 * k);
    if (frame)
        *frame = (long)f;
    return 0;
}

int savestate_load(const uint8_t *blob, size_t len, const char **why)
{
    const char *w = NULL;
    uint8_t sha[20];
    rom_sha1_bin(sha);
    if (savestate_peek(blob, len, NULL))
        w = "not a save state";
    else if (get32(blob + 8) != SAVESTATE_VERSION && get32(blob + 8) != 1)
        w = "a save state of another format version";
    else if (get32(blob + 12) > CT_MODULE_ABI || get32(blob + 12) < SAVESTATE_ABI_MIN)
        w = "a save state from another engine version (module ABI)";
    else if (memcmp(blob + 16, sha, 20))
        w = "a save state for another ROM";
    if (w) {
        if (why)
            *why = w;
        return -1;
    }
    /* Twice: a check of every section's layout that changes nothing, then
       the load, so a state from another build leaves the machine as it was. */
    for (int pass = 2; pass >= 1; pass--) {
        ss_io s = {0};
        s.loading = pass;
        s.version = get32(blob + 8);
        s.in = blob;
        s.in_len = len;
        s.pos = HEAD_LEN;
        walk(&s);
        if (!s.err && s.pos != len)
            s.err = "the state has trailing data";
        if (s.err) {
            if (why)
                *why = s.err;
            return -1;
        }
    }
    return 0;
}

/* ---- files ---- */

static int fsync_dir_of(const char *path)
{
    char dir[4096];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash)
        slash[slash == dir] = 0;
    else
        snprintf(dir, sizeof dir, ".");
    int fd = open(dir, O_RDONLY);
    if (fd < 0)
        return -1;
    int r = fsync(fd);
    close(fd);
    return r;
}

int savestate_write_file(const char *path, const char **why)
{
    size_t len;
    uint8_t *blob = savestate_save(&len, why);
    if (!blob)
        return -1;
    char tmp[4096 + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    size_t done = 0;
    while (fd >= 0 && done < len) {
        ssize_t n = write(fd, blob + done, len - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        done += (size_t)n;
    }
    free(blob);
    int ok = fd >= 0 && done == len && !fsync(fd);
    if (fd >= 0 && close(fd))
        ok = 0;
    if (!ok || rename(tmp, path)) {
        if (fd >= 0)
            remove(tmp);
        if (why)
            *why = strerror(errno);
        return -1;
    }
    fsync_dir_of(path);
    return 0;
}

int savestate_read_file(const char *path, const char **why)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (why)
            *why = strerror(errno);
        return -1;
    }
    size_t cap = 1u << 20, len = 0;
    uint8_t *buf = malloc(cap);
    while (buf) {
        size_t n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (len < cap)
            break;
        uint8_t *b = realloc(buf, cap * 2);
        if (!b) {
            free(buf);
            buf = NULL;
            break;
        }
        buf = b;
        cap *= 2;
    }
    int err = ferror(f);
    fclose(f);
    if (!buf || err) {
        free(buf);
        if (why)
            *why = buf ? "read error" : "out of memory";
        return -1;
    }
    int r = savestate_load(buf, len, why);
    free(buf);
    return r;
}
