/* SPDX-FileCopyrightText: 2026 ct-recomp contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * port: PS5 title build (docs/PS5.md). The engine is linked in from
 * vendor/lib/libct_title.a; its main is frontend/main.c.
 *
 * A title's process doesn't bind every libc import a payload gets: one
 * that isn't bound is left pointing at unmapped memory, and the first call
 * crashes (getcwd did, on 13.20). So this file:
 *
 *  - defines the libc functions known or likely to be unbound, on top of
 *    libkernel (which titles do get): getcwd, the environment, directory
 *    streams, and a few small ones. A definition here wins over the import.
 *  - checks every remaining import before main (ct_title_check_imports,
 *    a constructor), writes them all to /data/ct-recomp/imports.log using
 *    only libkernel, and names any unbound ones in a notification. The
 *    table comes from imports.gen.h (gen_imports.py).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <langinfo.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

/* ---- libkernel ---- */
typedef struct {
    char reserved[45];
    char message[3075];
} ct_notify_request;
int sceKernelSendNotificationRequest(int device, ct_notify_request *req, size_t size, int blocking);
int sceKernelOpen(const char *path, int flags, int mode);
long sceKernelWrite(int fd, const void *buf, size_t n);
int sceKernelClose(int fd);
int sceKernelMkdir(const char *path, int mode);
int sceKernelGetdents(int fd, char *buf, int n);

/* ---- small libc pieces ---- */
static size_t k_strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

/* Byte loops through volatile pointers: never turned into memset/memcpy
   calls, which are imports themselves (the import check runs before any
   import is known to work). */
static void k_copy(void *to, const void *from, size_t n)
{
    volatile char *d = to;
    const volatile char *s = from;
    while (n--)
        *d++ = *s++;
}

static int k_streq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

/* ---- the environment: a process-local table ---- */
#define CT_ENV_MAX 64
static char *env_keys[CT_ENV_MAX], *env_vals[CT_ENV_MAX];
static char env_store[8192];
static size_t env_used;

static char *env_dup(const char *s)
{
    size_t n = k_strlen(s) + 1;
    if (env_used + n > sizeof env_store)
        return NULL;
    char *p = env_store + env_used;
    k_copy(p, s, n);
    env_used += n;
    return p;
}

char *getenv(const char *name)
{
    for (int k = 0; k < CT_ENV_MAX; k++)
        if (env_keys[k] && k_streq(env_keys[k], name))
            return env_vals[k];
    return NULL;
}

int setenv(const char *name, const char *value, int overwrite)
{
    int free_slot = -1;
    for (int k = 0; k < CT_ENV_MAX; k++) {
        if (env_keys[k] && k_streq(env_keys[k], name)) {
            if (!overwrite)
                return 0;
            char *v = env_dup(value);
            if (!v)
                return errno = ENOMEM, -1;
            env_vals[k] = v;
            return 0;
        }
        if (!env_keys[k] && free_slot < 0)
            free_slot = k;
    }
    if (free_slot < 0)
        return errno = ENOMEM, -1;
    char *key = env_dup(name), *v = env_dup(value);
    if (!key || !v)
        return errno = ENOMEM, -1;
    env_keys[free_slot] = key;
    env_vals[free_slot] = v;
    return 0;
}

int unsetenv(const char *name)
{
    for (int k = 0; k < CT_ENV_MAX; k++)
        if (env_keys[k] && k_streq(env_keys[k], name))
            env_keys[k] = env_vals[k] = NULL;
    return 0;
}

/* ---- the working directory: the title's own folder ---- */
char *getcwd(char *buf, size_t size)
{
    static const char cwd[] = "/app0";
    if (!buf || size < sizeof cwd)
        return errno = ERANGE, NULL;
    k_copy(buf, cwd, sizeof cwd);
    return buf;
}

/* ---- directory streams, on getdents ----
   The kernel's getdents records are FreeBSD 11's (32-bit file number,
   16-bit record length, type, 8-bit name length, the name), whatever the
   SDK's struct dirent says, so they are parsed by hand and copied into
   one of our own. No O_DIRECTORY: its value differs between the headers
   and the kernel, and a plain read-only open of a directory works. */
struct _dirdesc {
    int fd, len, pos;
    char buf[8192];
    struct dirent out;
};
static struct _dirdesc dir_pool[4];
static int dir_used[4];
/* port: the last opendir's and readdir's kernel results, for the packs
   report (ps5_dir_status) */
static int dir_err[3];
static unsigned char dir_peek[48];
const unsigned char *ps5_dir_peek(void) { return dir_peek; }
void ps5_dir_status(int out[3])
{
    out[0] = dir_err[0];
    out[1] = dir_err[1];
    out[2] = dir_err[2];
}

DIR *opendir(const char *path)
{
    int slot = -1;
    for (int k = 0; k < 4; k++)
        if (!dir_used[k]) {
            slot = k;
            break;
        }
    if (slot < 0)
        return errno = EMFILE, NULL;
    /* FreeBSD's O_DIRECTORY (0x20000) first, then a plain read-only open */
    int fd = sceKernelOpen(path, 0x20000, 0);
    dir_err[0] = fd;
    if (fd < 0) {
        fd = sceKernelOpen(path, 0, 0);
        dir_err[1] = fd;
    }
    if (fd < 0)
        return errno = ENOENT, NULL;
    dir_used[slot] = 1;
    dir_pool[slot].fd = fd;
    dir_pool[slot].len = dir_pool[slot].pos = 0;
    return &dir_pool[slot];
}

/* One record at r (n bytes left): its name, length and the record's size,
   for the kernel's two possible layouts; 0 when neither fits. */
static int dir_layout = -1;   /* 0: FreeBSD 11 (32-bit ino), 1: FreeBSD 12+ (64-bit) */
static int dir_record(const unsigned char *r, int n, int layout, unsigned *reclen,
                      unsigned *namlen, int *nameoff, unsigned *type, unsigned long long *ino)
{
    if (layout == 0) {
        if (n < 8)
            return 0;
        *ino = r[0] | r[1] << 8 | r[2] << 16 | (unsigned long long)r[3] << 24;
        *reclen = r[4] | r[5] << 8;
        *type = r[6];
        *namlen = r[7];
        *nameoff = 8;
    } else {
        if (n < 24)
            return 0;
        *ino = 0;
        for (int k = 7; k >= 0; k--)
            *ino = *ino << 8 | r[k];
        *reclen = r[16] | r[17] << 8;
        *type = r[18];
        *namlen = r[20] | r[21] << 8;
        *nameoff = 24;
    }
    return *reclen >= (unsigned)*nameoff + *namlen && *reclen <= (unsigned)n &&
           (*reclen & 3) == 0 && *namlen < 256;
}

struct dirent *readdir(DIR *d)
{
    for (;;) {
        if (d->pos >= d->len) {
            int n = sceKernelGetdents(d->fd, d->buf, sizeof d->buf);
            dir_err[2] = n;
            if (n > 0)
                memcpy(dir_peek, d->buf, n < 48 ? (size_t)n : 48);
            if (n <= 0)
                return NULL;
            d->len = n;
            d->pos = 0;
        }
        const unsigned char *r = (const unsigned char *)d->buf + d->pos;
        int left = d->len - d->pos;
        unsigned reclen, namlen, type;
        int off;
        unsigned long long ino;
        if (dir_layout < 0) {   /* the first record says which layout it is */
            if (dir_record(r, left, 0, &reclen, &namlen, &off, &type, &ino))
                dir_layout = 0;
            else if (dir_record(r, left, 1, &reclen, &namlen, &off, &type, &ino))
                dir_layout = 1;
            else
                return NULL;
        }
        if (!dir_record(r, left, dir_layout, &reclen, &namlen, &off, &type, &ino))
            return NULL;
        d->pos += (int)reclen;
        if (!ino || !namlen)
            continue;
        if (namlen >= sizeof d->out.d_name)
            namlen = sizeof d->out.d_name - 1;
        memset(&d->out, 0, sizeof d->out);
        d->out.d_fileno = (__typeof__(d->out.d_fileno))ino;
        d->out.d_type = (uint8_t)type;
        memcpy(d->out.d_name, r + off, namlen);
        d->out.d_name[namlen] = 0;
        return &d->out;
    }
}

int closedir(DIR *d)
{
    sceKernelClose(d->fd);
    dir_used[d - dir_pool] = 0;
    return 0;
}

/* ---- functions not in the console's libc (localtime_r, the locale) ---- */
struct tm *localtime_r(const time_t *t, struct tm *out)
{
    struct tm *p = localtime(t);
    if (!p)
        return NULL;
    *out = *p;
    return out;
}

char *nl_langinfo(nl_item item)
{
    static char utf8[] = "UTF-8", empty[] = "";
    return item == CODESET ? utf8 : empty;
}

int ___mb_cur_max(void)
{
    return 1;
}

char *strcasestr(const char *s, const char *find)
{
    size_t n = k_strlen(find);
    for (; *s; s++)
        if (!strncasecmp(s, find, n))
            return (char *)s;
    return n ? NULL : (char *)s;
}

/* isatty and mkstemp: libScePosixForWebKit's, which a game process may
   not get: ours, on libkernel. */
int isatty(int fd)
{
    (void)fd;
    return errno = ENOTTY, 0;
}

int mkstemp(char *tmpl)
{
    size_t n = k_strlen(tmpl);
    if (n < 6)
        return errno = EINVAL, -1;
    static unsigned seed = 0x9e3779b9u;
    for (int tries = 0; tries < 100; tries++) {
        unsigned v = seed = seed * 1664525u + 1013904223u + (unsigned)tries;
        for (size_t k = n - 6; k < n; k++, v /= 36)
            tmpl[k] = "0123456789abcdefghijklmnopqrstuvwxyz"[v % 36];
        int fd = sceKernelOpen(tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0)
            return fd;
    }
    return errno = EEXIST, -1;
}

pid_t fork(void)
{
    return errno = ENOSYS, -1;
}

int link(const char *a, const char *b)
{
    (void)a, (void)b;
    return errno = ENOSYS, -1;
}

/* ---- the heap ----
   In a title, libSceLibcInternal's malloc runs out after a few MB (13.20:
   "Out of memory" for SDL's 8 MB window surface, after the 4 MB ROM). So
   malloc and friends are ours, over one large anonymous mapping: segregated
   free lists of boundary-tagged blocks, 16-byte aligned, coalescing on
   free, behind a spinlock. strdup too, so nothing we free came from the
   console's heap. */
void *mmap(void *addr, size_t len, int prot, int flags, int fd, long off);

typedef struct ct_block {
    size_t prev_size;            /* size of the block before, if it's free */
    size_t size;                 /* this block, header included; bit 0: in use */
    struct ct_block *next, *prev;   /* free blocks only (payload) */
} ct_block;

#define CT_HDR 16u
#define CT_MIN 32u
#define CT_BINS 48
#define CT_USED ((size_t)1)
static ct_block *bins[CT_BINS];
static char *heap_lo, *heap_hi;
static size_t heap_size;
static volatile int heap_lock;
static int heap_ready;

static void lock(void)
{
    while (__atomic_exchange_n(&heap_lock, 1, __ATOMIC_ACQUIRE))
        while (heap_lock)
            __builtin_ia32_pause();
}

static void unlock(void)
{
    __atomic_store_n(&heap_lock, 0, __ATOMIC_RELEASE);
}

static int bin_of(size_t n)
{
    int b = 0;
    while (b < CT_BINS - 1 && ((size_t)CT_MIN << (b + 1)) <= n)
        b++;
    return b;
}

static size_t bsize(const ct_block *b) { return b->size & ~CT_USED; }
static ct_block *after(ct_block *b) { return (ct_block *)((char *)b + bsize(b)); }

static void unlink_free(ct_block *b)
{
    if (b->prev)
        b->prev->next = b->next;
    else
        bins[bin_of(bsize(b))] = b->next;
    if (b->next)
        b->next->prev = b->prev;
}

static void insert_free(ct_block *b)
{
    int i = bin_of(bsize(b));
    b->prev = NULL;
    b->next = bins[i];
    if (bins[i])
        bins[i]->prev = b;
    bins[i] = b;
    ct_block *n = after(b);
    if ((char *)n < heap_hi)
        n->prev_size = bsize(b);
}

static int heap_init(void)
{
    /* 1 GB, else the most the title gets down to 64 MB: the game itself
       needs well under 256 MB. */
    for (size_t want = (size_t)1 << 30; want >= ((size_t)64 << 20); want >>= 1) {
        void *p = mmap(NULL, want, 3 /* PROT_READ|PROT_WRITE */,
                       0x1000 | 0x2 /* MAP_ANON|MAP_PRIVATE */, -1, 0);
        if (p && p != (void *)-1) {
            heap_lo = p;
            heap_size = want;
            heap_hi = heap_lo + want - CT_HDR;   /* a used sentinel ends it */
            ct_block *first = (ct_block *)heap_lo;
            first->prev_size = 0;
            first->size = (size_t)(heap_hi - heap_lo);
            ct_block *end = (ct_block *)heap_hi;
            end->size = CT_USED;
            insert_free(first);
            end->prev_size = bsize(first);
            heap_ready = 1;
            return 0;
        }
    }
    return -1;
}

static void *alloc_locked(size_t n)
{
    if (!heap_ready && heap_init())
        return NULL;
    if (n > heap_size)
        return NULL;
    size_t need = (n + CT_HDR + 15) & ~(size_t)15;
    if (need < CT_MIN)
        need = CT_MIN;
    for (int i = bin_of(need); i < CT_BINS; i++)
        for (ct_block *b = bins[i]; b; b = b->next) {
            if (bsize(b) < need)
                continue;
            unlink_free(b);
            size_t rest = bsize(b) - need;
            if (rest >= CT_MIN) {
                b->size = need;
                ct_block *r = after(b);
                r->prev_size = need;
                r->size = rest;
                insert_free(r);
            }
            b->size |= CT_USED;
            return (char *)b + CT_HDR;
        }
    return NULL;
}

static void free_locked(void *p)
{
    ct_block *b = (ct_block *)((char *)p - CT_HDR);
    b->size &= ~CT_USED;
    ct_block *n = after(b);
    if ((char *)n < heap_hi && !(n->size & CT_USED)) {   /* merge the next one */
        unlink_free(n);
        b->size += bsize(n);
    }
    if ((char *)b > heap_lo) {   /* merge the previous one, if free */
        ct_block *pb = (ct_block *)((char *)b - b->prev_size);
        if (b->prev_size && !(pb->size & CT_USED)) {
            unlink_free(pb);
            pb->size += bsize(b);
            b = pb;
        }
    }
    insert_free(b);
}

static int ours(const void *p)
{
    return (const char *)p >= heap_lo && (const char *)p < heap_hi;
}

void *malloc(size_t n)
{
    lock();
    void *p = alloc_locked(n ? n : 1);
    unlock();
    if (!p)
        errno = ENOMEM;
    return p;
}

void free(void *p)
{
    if (!p || !ours(p))   /* not ours: from the console's heap; leave it */
        return;
    lock();
    free_locked(p);
    unlock();
}

void *calloc(size_t a, size_t b)
{
    if (b && a > (size_t)-1 / b)
        return errno = ENOMEM, NULL;
    void *p = malloc(a * b);
    if (p)
        memset(p, 0, a * b);
    return p;
}

void *realloc(void *p, size_t n)
{
    if (!p)
        return malloc(n);
    if (!n) {
        free(p);
        return NULL;
    }
    size_t have = ours(p) ? bsize((ct_block *)((char *)p - CT_HDR)) - CT_HDR : 0;
    if (have >= n)
        return p;
    void *q = malloc(n);
    if (q && have)
        memcpy(q, p, have);
    if (q)
        free(p);
    return q;
}

int posix_memalign(void **out, size_t align, size_t n)
{
    if (align < 16)
        align = 16;
    if (align & (align - 1))
        return 22;   /* EINVAL */
    lock();
    char *raw = alloc_locked(n + align + CT_MIN);
    if (!raw) {
        unlock();
        return 12;   /* ENOMEM */
    }
    uintptr_t at = ((uintptr_t)raw + CT_MIN + align - 1) & ~(uintptr_t)(align - 1);
    if (((uintptr_t)raw & (align - 1)) == 0)
        at = (uintptr_t)raw;
    if (at != (uintptr_t)raw) {   /* give the front back as its own free block */
        ct_block *b = (ct_block *)(raw - CT_HDR);
        size_t front = (at - CT_HDR) - (uintptr_t)b, total = bsize(b);
        ct_block *a = (ct_block *)(at - CT_HDR);
        a->prev_size = front;
        a->size = (total - front) | CT_USED;
        b->size = front | CT_USED;
        ct_block *n2 = after(a);
        if ((char *)n2 < heap_hi)
            n2->prev_size = bsize(a);
        free_locked(raw);   /* sets a->prev_size, merged or not */
    }
    unlock();
    *out = (void *)at;
    return 0;
}

size_t malloc_usable_size(void *p)   /* Mesa asks */
{
    if (!p || !ours(p))
        return 0;
    return bsize((ct_block *)((char *)p - CT_HDR)) - CT_HDR;
}

void *aligned_alloc(size_t align, size_t n)
{
    void *p;
    return posix_memalign(&p, align, n) ? NULL : p;
}

void *memalign(size_t align, size_t n)
{
    return aligned_alloc(align, n);
}

char *strdup(const char *s)
{
    size_t n = k_strlen(s) + 1;
    char *p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

/* ---- the keyboard and IME dialog: not bound in a title (13.20) ----
   SDL's PS5 video driver calls these at start-up; failing them leaves SDL
   without a keyboard, which a console game doesn't need. */
int sceKeyboardInit(void) { return -1; }
int sceKeyboardOpen(int user, int type, int index, void *param)
{
    (void)user, (void)type, (void)index, (void)param;
    return -1;
}
int sceKeyboardClose(int handle) { (void)handle; return -1; }
int sceKeyboardReadState(int handle, void *data)
{
    (void)handle, (void)data;
    return -1;
}
int sceImeDialogInit(const void *param, void *extended)
{
    (void)param, (void)extended;
    return -1;
}
int sceImeDialogGetStatus(void) { return 0; }   /* SCE_IME_DIALOG_STATUS_NONE */
int sceImeDialogGetResult(void *result) { (void)result; return -1; }
int sceImeDialogTerm(void) { return -1; }

/* ---- the import check ---- */
#include "imports.gen.h"   /* CT_IMPORTS(X): X(index, "name", "module") per import */

#define CT_DECL(i, name, module) extern char ct_import_##i __asm__(name);
CT_IMPORTS(CT_DECL)
#define CT_ENTRY(i, name, module) {name, module, &ct_import_##i},
static const struct {
    const char *name, *module;
    const void *addr;
} imports[] = {CT_IMPORTS(CT_ENTRY)};

static void kwrite(int fd, const char *s)
{
    sceKernelWrite(fd, s, k_strlen(s));
}

static void khex(char *out, uintptr_t v)
{
    static const char hex[] = "0123456789abcdef";
    out[0] = '0', out[1] = 'x';
    for (int k = 0; k < 16; k++)
        out[2 + k] = hex[(v >> (60 - 4 * k)) & 15];
    out[18] = 0;
}

/* Bound or not: on 13.20 the system modules a title imports from load at
   0x8_0000_0000 and up (every bound import in the first log was there),
   and the title itself at 0x40_0000. An import the loader didn't bind was
   left at 0 (sceKeyboardInit) or at a stray low value (getcwd,
   0x1_0026_c946). sceKernelVirtualQuery can't tell: it failed for every
   import, bound ones included. */
extern char __ehdr_start[];   /* the start of this program (lld) */
static int bound(const void *p)
{
    uintptr_t a = (uintptr_t)p, self = (uintptr_t)__ehdr_start;
    return a >= 0x800000000ull || (a >= self && a < self + 0x40000000ull);
}

__attribute__((constructor)) static void ct_title_check_imports(void)
{
    sceKernelMkdir("/data/ct-recomp", 0777);
    int fd = sceKernelOpen("/data/ct-recomp/imports.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    size_t bad = 0, total = sizeof imports / sizeof *imports;
    for (size_t k = 0; k < total; k++) {
        int ok = bound(imports[k].addr);
        bad += !ok;
        if (fd >= 0) {
            char h[19];
            khex(h, (uintptr_t)imports[k].addr);
            kwrite(fd, ok ? "ok   " : "BAD  ");
            kwrite(fd, h);
            kwrite(fd, " ");
            kwrite(fd, imports[k].module);
            kwrite(fd, " ");
            kwrite(fd, imports[k].name);
            kwrite(fd, "\n");
        }
    }
    if (fd >= 0)
        sceKernelClose(fd);
    if (bad) {
        static ct_notify_request req;   /* static: zeroed without memset */
        static const char head[] = "CT Recomp: unbound system functions:";
        size_t at = sizeof head - 1;
        k_copy(req.message, head, at);
        for (size_t k = 0; k < total; k++) {
            size_t n = k_strlen(imports[k].name);
            if (bound(imports[k].addr) || at + n + 2 >= 400)
                continue;
            req.message[at++] = ' ';
            k_copy(req.message + at, imports[k].name, n);
            at += n;
        }
        sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
    }
}
