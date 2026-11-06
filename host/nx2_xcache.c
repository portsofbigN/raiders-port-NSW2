// On-disk cache of the shader translator's output (nx2_xlate_program), so a shader translated once is not translated
// again in later sessions: the 2 ms translation of each new shader was most of the hitch when an effect appears for
// the first time (the driver's own compile is cached by the driver).
//
// A translation depends on the stages' code and options (the key: two 64-bit hashes of them) and on what the
// translator asked the game while it ran: constant buffer words, texture types. Those questions and their answers
// are stored with the result, and a cached result is used only when the same questions get the same answers now
// (texture handles may differ between sessions: what is compared is the type of the texture each one names).
//
// The file is shader_cache.bin next to the program; it starts over when the translator changes (NX2_XLATE_ID, from
// build.py), the old file kept under another name. NX2_XCACHE=0 turns the cache off.
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "host.h"
#include "nx2_xlate.h"

#ifndef NX2_XLATE_ID
#define NX2_XLATE_ID 0ull
#endif
#define FORMAT 1u
#define REC_MAGIC 0x43455258u   // "XREC"

typedef struct { uint32_t kind_stage, a, b, r; } Q;   // kind 0 cbuf (a index, b offset), 1 texture type, 2 integer
typedef struct Entry {
    uint64_t k1, k2;
    uint32_t nq, n;
    Q *q;
    Nx2XStage *st;   // spirv owned
    struct Entry *next;
} Entry;

static CRITICAL_SECTION lock;
static volatile LONG init_state;
static int enabled;
static Entry *table[8192];
static FILE *file;
static uint64_t hits, misses, refused, loaded;

// recording of the translator's questions
static Q *rq;
static uint32_t rn, rcap;
static const Nx2XOptions *rorig;

static void push(uint32_t ks, uint32_t a, uint32_t b, uint32_t r) {
    if (rn == rcap) {
        uint32_t nc = rcap ? rcap * 2 : 64;
        Q *nq = realloc(rq, nc * sizeof *nq);
        if (!nq) return;
        rq = nq, rcap = nc;
    }
    rq[rn++] = (Q){ks, a, b, r};
}
static uint32_t rec_cbuf(void *u, int stage, uint32_t index, uint32_t offset) {
    (void)u;
    uint32_t r = rorig->cbuf_value(rorig->user, stage, index, offset);
    push(0u | (uint32_t)stage << 8, index, offset, r);
    return r;
}
static int rec_type(void *u, int stage, uint32_t handle) {
    (void)u;
    int r = rorig->texture_type(rorig->user, stage, handle);
    push(1u | (uint32_t)stage << 8, handle, 0, (uint32_t)r);
    return r;
}
static int rec_int(void *u, int stage, uint32_t handle) {
    (void)u;
    int r = rorig->texture_integer(rorig->user, stage, handle);
    push(2u | (uint32_t)stage << 8, handle, 0, (uint32_t)r);
    return r;
}

// The same questions now: 1 when every answer is the same. Handles (constant buffer 2) are mapped, not compared.
static int still_valid(const Entry *e, const Nx2XOptions *o) {
    uint32_t from[64], to[64];
    int nmap = 0;
    for (uint32_t i = 0; i < e->nq; i++) {
        const Q *q = &e->q[i];
        int kind = (int)(q->kind_stage & 0xff), stage = (int)(q->kind_stage >> 8);
        if (kind == 0) {
            uint32_t v = o->cbuf_value(o->user, stage, q->a, q->b);
            if (q->a == 2) {
                if (nmap < 64) from[nmap] = q->r, to[nmap] = v, nmap++;
                continue;
            }
            if (v != q->r) return 0;
            continue;
        }
        uint32_t h = q->a;
        for (int k = nmap - 1; k >= 0; k--)
            if (from[k] == h) {
                h = to[k];
                break;
            }
        uint32_t v = (uint32_t)(kind == 1 ? o->texture_type(o->user, stage, h) : o->texture_integer(o->user, stage, h));
        if (v != q->r) return 0;
    }
    return 1;
}

static void hash_bytes(uint64_t *h1, uint64_t *h2, const void *p, size_t n) {
    const uint8_t *b = p;
    uint64_t a = *h1, c = *h2;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, b + i, 8);
        a = (a ^ w) * 0x9E3779B97F4A7C15ull, a ^= a >> 29;
        c = (c + w) * 0xC2B2AE3D27D4EB4Full, c ^= c >> 31;
    }
    for (; i < n; i++) {
        a = (a ^ b[i]) * 0x100000001B3ull;
        c = (c + b[i]) * 0xD6E8FEB86659FD93ull, c ^= c >> 32;
    }
    *h1 = a ^ (uint64_t)n, *h2 = c + (uint64_t)n;
}

// ---- stages <-> bytes
static size_t stage_bytes(const Nx2XStage *s) {
    return offsetof(Nx2XStage, desc) + (size_t)s->ndesc * sizeof(Nx2XDesc) +
           (offsetof(Nx2XStage, error) - offsetof(Nx2XStage, loads_generic)) + s->words * 4;
}
static uint8_t *put_stage(uint8_t *o, const Nx2XStage *s) {
    Nx2XStage t = *s;
    t.spirv = NULL;
    memcpy(o, &t, offsetof(Nx2XStage, desc)), o += offsetof(Nx2XStage, desc);
    memcpy(o, s->desc, (size_t)s->ndesc * sizeof(Nx2XDesc)), o += (size_t)s->ndesc * sizeof(Nx2XDesc);
    size_t tail = offsetof(Nx2XStage, error) - offsetof(Nx2XStage, loads_generic);
    memcpy(o, (const uint8_t *)s + offsetof(Nx2XStage, loads_generic), tail), o += tail;
    memcpy(o, s->spirv, s->words * 4), o += s->words * 4;
    return o;
}
// 0 when the bytes do not hold a whole stage
static const uint8_t *get_stage(const uint8_t *p, const uint8_t *end, Nx2XStage *s) {
    memset(s, 0, sizeof *s);
    if ((size_t)(end - p) < offsetof(Nx2XStage, desc)) return NULL;
    memcpy(s, p, offsetof(Nx2XStage, desc)), p += offsetof(Nx2XStage, desc);
    s->spirv = NULL;
    size_t tail = offsetof(Nx2XStage, error) - offsetof(Nx2XStage, loads_generic);
    if (s->ndesc < 0 || s->ndesc > 96 || s->words > (64u << 20) ||
        (size_t)(end - p) < (size_t)s->ndesc * sizeof(Nx2XDesc) + tail + s->words * 4)
        return NULL;
    memcpy(s->desc, p, (size_t)s->ndesc * sizeof(Nx2XDesc)), p += (size_t)s->ndesc * sizeof(Nx2XDesc);
    memcpy((uint8_t *)s + offsetof(Nx2XStage, loads_generic), p, tail), p += tail;
    s->spirv = malloc(s->words * 4 ? s->words * 4 : 4);
    if (!s->spirv) return NULL;
    memcpy(s->spirv, p, s->words * 4), p += s->words * 4;
    return p;
}

static void insert(Entry *e) {
    Entry **b = &table[e->k1 % 8192];
    e->next = *b;
    *b = e;
}

// A record: magic, length of the rest, k1, k2, nq, n, questions, stages. NULL when the bytes do not hold one.
static const uint8_t *load_record(const uint8_t *p, const uint8_t *end) {
    uint32_t magic, len;
    if (end - p < 8) return NULL;
    memcpy(&magic, p, 4), memcpy(&len, p + 4, 4);
    if (magic != REC_MAGIC || (size_t)(end - p - 8) < len || len < 24) return NULL;
    const uint8_t *r = p + 8, *rend = r + len;
    Entry *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    memcpy(&e->k1, r, 8), memcpy(&e->k2, r + 8, 8), memcpy(&e->nq, r + 16, 4), memcpy(&e->n, r + 20, 4);
    r += 24;
    if (e->n < 1 || e->n > 5 || e->nq > (1u << 20) || (size_t)(rend - r) < e->nq * sizeof(Q)) goto bad;
    e->q = malloc(e->nq * sizeof(Q) + 1);
    e->st = calloc(e->n, sizeof(Nx2XStage));
    if (!e->q || !e->st) goto bad;
    memcpy(e->q, r, e->nq * sizeof(Q)), r += e->nq * sizeof(Q);
    for (uint32_t i = 0; i < e->n; i++)
        if (!(r = get_stage(r, rend, &e->st[i]))) goto bad;
    if (r != rend) goto bad;
    insert(e);
    return rend;
bad:
    if (e->st)
        for (uint32_t i = 0; i < e->n; i++) free(e->st[i].spirv);
    free(e->st), free(e->q), free(e);
    return NULL;
}

static void cache_path(char *out, size_t n, const char *name) {
    char exe[MAX_PATH];
    DWORD k = GetModuleFileNameA(NULL, exe, sizeof exe);
    if (!k || k >= sizeof exe) exe[0] = 0;
    char *slash = strrchr(exe, '\\');
    if (slash) slash[1] = 0;
    else exe[0] = 0;
    snprintf(out, n, "%s%s", exe, name);
}

static void set_aside(const char *path, const char *why) {   // never deleted: renamed next to it
    char other[MAX_PATH + 64];
    snprintf(other, sizeof other, "%.*s.%s-%lu.bin", (int)(strlen(path) - 4), path, why, (unsigned long)GetTickCount());
    if (MoveFileA(path, other)) rt_log("xcache: %s; the old cache kept as %s", why, other);
}

static void open_cache(void) {
    const char *e = getenv("NX2_XCACHE");
    if (e && *e == '0') return;
    char path[MAX_PATH + 32];
    cache_path(path, sizeof path, "shader_cache.bin");
    struct { char magic[8]; uint32_t format, pad; uint64_t id; } hdr = {{'N', 'X', '2', 'X', 'C', 'A', 'C', 'H'}, FORMAT, 0,
                                                                       NX2_XLATE_ID};
    FILE *f = fopen(path, "rb");
    long good = 0;
    int rewrite = 0;
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *all = size > 0 ? malloc((size_t)size) : NULL;
        if (all && fread(all, 1, (size_t)size, f) == (size_t)size && size >= (long)sizeof hdr &&
            !memcmp(all, &hdr, sizeof hdr)) {
            const uint8_t *p = all + sizeof hdr, *end = all + size, *q;
            while (p < end && (q = load_record(p, end))) p = q, loaded++;
            good = (long)(p - all);
            if (p != end) rewrite = 2;   // a damaged end (the program stopped while writing)
        } else
            rewrite = 1;   // another translator or format
        fclose(f);
        if (rewrite) {
            set_aside(path, rewrite == 1 ? "translator-changed" : "damaged-end");
            if (rewrite == 2 && all) {   // the whole records go to the new file
                FILE *nf = fopen(path, "wb");
                if (nf) fwrite(all, 1, (size_t)good, nf), fclose(nf);
            }
            if (rewrite == 1) {
                for (int i = 0; i < 8192; i++) table[i] = NULL;   // nothing was loaded from it
                loaded = 0;
            }
        }
        free(all);
    }
    file = fopen(path, "ab");
    if (!file) {
        rt_log("xcache: %s cannot be written; translations are not kept", path);
        enabled = 1;
        return;
    }
    fseek(file, 0, SEEK_END);   // in append mode the position is only set by the first write
    if (ftell(file) == 0) fwrite(&hdr, sizeof hdr, 1, file), fflush(file);
    rt_log("xcache: %llu translated programs loaded from %s", (unsigned long long)loaded, path);
    enabled = 1;
}

static void save(const Entry *e) {
    if (!file) return;
    size_t len = 24 + e->nq * sizeof(Q);
    for (uint32_t i = 0; i < e->n; i++) len += stage_bytes(&e->st[i]);
    uint8_t *buf = malloc(8 + len), *o = buf;
    if (!buf) return;
    uint32_t magic = REC_MAGIC, l32 = (uint32_t)len;
    memcpy(o, &magic, 4), memcpy(o + 4, &l32, 4), o += 8;
    memcpy(o, &e->k1, 8), memcpy(o + 8, &e->k2, 8), memcpy(o + 16, &e->nq, 4), memcpy(o + 20, &e->n, 4), o += 24;
    memcpy(o, e->q, e->nq * sizeof(Q)), o += e->nq * sizeof(Q);
    for (uint32_t i = 0; i < e->n; i++) o = put_stage(o, &e->st[i]);
    fwrite(buf, 1, (size_t)(o - buf), file);
    fflush(file);
    free(buf);
}

// nx2_xlate_program through the cache. len[i]: the bytes of stage i that hold its code (hashed).
int nx2_xcache_translate(int n, const void *const *code, const size_t *size, const size_t *len, int compute,
                         const Nx2XOptions *opt, Nx2XStage *out) {
    if (InterlockedCompareExchange(&init_state, 1, 0) == 0) {
        InitializeCriticalSection(&lock);
        InterlockedExchange(&init_state, 2);
    }
    while (init_state != 2) Sleep(0);
    EnterCriticalSection(&lock);
    static int opened;
    if (!opened) opened = 1, open_cache();
    if (!enabled || !opt->cbuf_value || !opt->texture_type || !opt->texture_integer) {
        LeaveCriticalSection(&lock);
        return nx2_xlate_program(n, code, size, compute, opt, out);
    }
    uint64_t k1 = 0x243F6A8885A308D3ull ^ NX2_XLATE_ID, k2 = 0x13198A2E03707344ull + FORMAT;
    uint32_t head[2] = {(uint32_t)n, (uint32_t)compute};
    hash_bytes(&k1, &k2, head, sizeof head);
    for (int i = 0; i < n; i++) hash_bytes(&k1, &k2, code[i], len[i]);
    hash_bytes(&k1, &k2, (const uint8_t *)opt + offsetof(Nx2XOptions, attrib_types),
               sizeof *opt - offsetof(Nx2XOptions, attrib_types));
    for (Entry *e = table[k1 % 8192]; e; e = e->next) {
        if (e->k1 != k1 || e->k2 != k2 || (int)e->n != n) continue;
        if (!still_valid(e, opt)) {
            refused++;
            continue;
        }
        int ok = 1;
        for (int i = 0; i < n; i++) {
            out[i] = e->st[i];
            out[i].spirv = malloc(e->st[i].words * 4 ? e->st[i].words * 4 : 4);
            if (!out[i].spirv) ok = 0;
            else memcpy(out[i].spirv, e->st[i].spirv, e->st[i].words * 4);
        }
        if (!ok) {
            nx2_xlate_free(out, n);
            break;
        }
        hits++;
        static int verify = -1;   // NX2_XCACHE_VERIFY=1 (debug): translate anyway and compare
        if (verify < 0) verify = getenv("NX2_XCACHE_VERIFY") && *getenv("NX2_XCACHE_VERIFY") == '1';
        if (verify) {
            Nx2XStage *fresh = calloc((size_t)n, sizeof *fresh);
            if (fresh && !nx2_xlate_program(n, code, size, compute, opt, fresh)) {
                int same = 1;
                for (int i = 0; i < n; i++)
                    if (fresh[i].words != out[i].words || memcmp(fresh[i].spirv, out[i].spirv, out[i].words * 4) ||
                        fresh[i].ndesc != out[i].ndesc ||
                        memcmp(fresh[i].desc, out[i].desc, (size_t)out[i].ndesc * sizeof(Nx2XDesc)))
                        same = 0;
                static uint64_t checked, differ;
                checked++, differ += !same;
                if (!same || !(checked % 100))
                    rt_log("xcache verify: %llu checked, %llu differ%s", (unsigned long long)checked,
                           (unsigned long long)differ, same ? "" : " (this one)");
                nx2_xlate_free(fresh, n);
            }
            free(fresh);
        }
        LeaveCriticalSection(&lock);
        return 0;
    }
    // translate, with the questions recorded
    Nx2XOptions o = *opt;
    rorig = opt, rn = 0;
    o.cbuf_value = rec_cbuf, o.texture_type = rec_type, o.texture_integer = rec_int;
    int r = nx2_xlate_program(n, code, size, compute, &o, out);
    misses++;
    if (!r) {
        Entry *e = calloc(1, sizeof *e);
        if (e) {
            e->k1 = k1, e->k2 = k2, e->nq = rn, e->n = (uint32_t)n;
            e->q = malloc(rn * sizeof(Q) + 1);
            e->st = calloc((size_t)n, sizeof(Nx2XStage));
            int ok = e->q && e->st;
            if (ok) memcpy(e->q, rq, rn * sizeof(Q));
            for (int i = 0; ok && i < n; i++) {
                e->st[i] = out[i];
                e->st[i].spirv = malloc(out[i].words * 4 ? out[i].words * 4 : 4);
                if (!e->st[i].spirv) ok = 0;
                else memcpy(e->st[i].spirv, out[i].spirv, out[i].words * 4);
            }
            if (ok) {
                insert(e);
                save(e);
            } else {
                if (e->st)
                    for (int i = 0; i < n; i++) free(e->st[i].spirv);
                free(e->st), free(e->q), free(e);
            }
        }
    }
    LeaveCriticalSection(&lock);
    return r;
}

void nx2_xcache_stats(uint64_t *h, uint64_t *m, uint64_t *rf, uint64_t *ld) {
    *h = hits, *m = misses, *rf = refused, *ld = loaded;
}
