// Read-only game RomFS served from an extracted directory tree, in place of TotK Recomp's romfs.c (which reads a
// romfs.bin image through a prebuilt index). Same interface (fs.h), so fs_host.c is unchanged.
//
// Root: rt_config()->romfs, which tools/build.py points at the update's romfs/. That tree is the whole merged RomFS:
// unchanged files are hard links to the base's, the 135 changed ones are the update's own copies.
//
// fs_init walks the tree once and keeps a sorted index of files (path, size) and of directories, so lookups and
// GetEntryType need no file system calls. Reads open the host file per guest handle and read positionally, so threads
// read in parallel. No overlays, mods or resource-size-table rebuild (TotK features); they can come back later.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs.h"
#include "host.h"
#include "platform/platform.h"
#include "romfs_path.h"

#define NH 4096                  // open RomFS files at once
#define READ_CHUNK (1u << 30)    // plat_file_pread takes 32-bit sizes

typedef struct { char *path; uint64_t size; } Entry;

static char root[PLAT_PATH_MAX * 3];
static Entry *files;
static uint64_t nfiles, cap_files;
static char **dirs;              // "a/b/" with the trailing slash; "" is the root
static uint64_t ndirs, cap_dirs;
static char mount_name[64];
static PlatMutex lock;
static PlatFile *handles[NH];
static uint64_t handle_entry[NH];   // index + 1
static PlatOnce once;

static void push_file(const char *rel, uint64_t size) {
    if (nfiles == cap_files) {
        cap_files = cap_files ? cap_files * 2 : 65536;
        files = realloc(files, cap_files * sizeof *files);
        if (!files) rt_fatal("romfs: index allocation");
    }
    files[nfiles++] = (Entry){strdup(rel), size};
}

static void push_dir(const char *rel) {
    if (ndirs == cap_dirs) {
        cap_dirs = cap_dirs ? cap_dirs * 2 : 4096;
        dirs = realloc(dirs, cap_dirs * sizeof *dirs);
        if (!dirs) rt_fatal("romfs: index allocation");
    }
    dirs[ndirs++] = strdup(rel);
}

typedef struct { char rel[4096]; size_t n; } Walk;
static void walk(Walk *w);

static int walk_cb(const char *name, int kind, uint64_t size, void *ctx) {
    Walk *w = ctx;
    size_t len = strlen(name);
    if (w->n + len + 2 >= sizeof w->rel) rt_fatal("romfs: path too long under %s", w->rel);
    memcpy(w->rel + w->n, name, len + 1);
    if (kind == PLAT_FS_DIR) {
        size_t keep = w->n;
        w->n += len;
        w->rel[w->n++] = '/';
        w->rel[w->n] = 0;
        push_dir(w->rel);
        walk(w);
        w->n = keep;
    } else if (kind == PLAT_FS_FILE) {
        push_file(w->rel, size);
    }
    w->rel[w->n] = 0;
    return 1;
}

static void walk(Walk *w) {
    char abs[sizeof root + sizeof w->rel + 2];
    snprintf(abs, sizeof abs, "%s/%s", root, w->rel);
    if (plat_fs_list(abs, walk_cb, w)) rt_fatal("romfs: cannot list %s", abs);
}

static int by_path(const void *a, const void *b) { return strcmp(((const Entry *)a)->path, ((const Entry *)b)->path); }
static int by_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void init_once(void *arg) {
    (void)arg;
    const char *r = rt_config()->romfs;
    if (!r || !plat_dir_exists(r)) rt_fatal("romfs: directory %s does not exist", r ? r : "(none)");
    snprintf(root, sizeof root, "%s", r);
    uint64_t t0 = plat_now_ns();
    static Walk w;
    push_dir("");
    walk(&w);
    qsort(files, nfiles, sizeof *files, by_path);
    qsort(dirs, ndirs, sizeof *dirs, by_str);
    uint64_t bytes = 0;
    for (uint64_t i = 0; i < nfiles; i++) bytes += files[i].size;
    rt_log("romfs: %s: %llu files, %llu directories, %.1f GiB, indexed in %.2f s", root, (unsigned long long)nfiles,
           (unsigned long long)ndirs, bytes / 1073741824.0, (plat_now_ns() - t0) / 1e9);
}

void fs_init(void) { plat_once(&once, init_once, NULL); }

void fs_trace(const char *what, const char *path) {
    if (rt_trace & RT_TRACE_FS) rt_log("fs %s %s", what, path);
}

static uint64_t file_index(const char *rel) {
    Entry key = {(char *)rel, 0};
    Entry *e = bsearch(&key, files, nfiles, sizeof *files, by_path);
    return e ? (uint64_t)(e - files) : UINT64_MAX;
}

static int dir_exists(const char *rel_with_slash) {
    const char *k = rel_with_slash;
    return bsearch(&k, dirs, ndirs, sizeof *dirs, by_str) != NULL;
}

uint64_t romfs_file_count(void) { return nfiles; }

const char *romfs_entry(uint64_t i, uint64_t *offset, uint64_t *size) {
    if (i >= nfiles) return NULL;
    if (offset) *offset = 0;
    if (size) *size = files[i].size;
    return files[i].path;
}

const char *romfs_image_path(void) { return root; }
size_t romfs_overlay_count(void) { return 0; }
int romfs_patches_loaded(void) { return 0; }
const char *romfs_overlay(size_t k, const char **file, uint64_t *size, int *is_added) {
    (void)k, (void)file, (void)size, (void)is_added;
    return NULL;
}
const char *romfs_prompts(void) { return NULL; }
int romfs_prompts_ready(const char *folder) { (void)folder; return 0; }
int romfs_prompts_off(const char *folder) { (void)folder; return ROMFS_PROMPTS_OK; }

int romfs_owns(const char *path) {
    size_t n = strlen(mount_name);
    return n && path && !strncmp(path, mount_name, n) && path[n] == ':';
}

int romfs_handle(uint64_t h) { return h > ROMFS_HANDLE_BASE && h < ROMFS_HANDLE_BASE + NH; }

uint32_t romfs_mount(const char *name) {
    fs_init();
    if (!name || !*name || strlen(name) >= sizeof mount_name || strchr(name, '/') || strchr(name, ':'))
        return FS_R_INVALID;
    plat_mutex_lock(&lock);
    if (*mount_name && strcmp(name, mount_name)) rt_fatal("second RomFS mount %s (%s is mounted)", name, mount_name);
    strcpy(mount_name, name);
    plat_mutex_unlock(&lock);
    rt_log("fs mount %s: -> RomFS %s", name, root);
    return 0;
}

uint32_t romfs_open(uint64_t *out, const char *path, int mode) {
    fs_trace("rom", path);
    if (!out || mode != FS_OPEN_READ) return FS_R_UNSUPPORTED_OPEN;
    const char *p = romfs_path_relative(mount_name, path);
    if (!p) return FS_R_PATH_NOT_FOUND;
    uint64_t i = file_index(p);
    if (i == UINT64_MAX) return FS_R_PATH_NOT_FOUND;
    char abs[sizeof root + 4096];
    snprintf(abs, sizeof abs, "%s/%s", root, p);
    PlatFile *f = plat_file_open(abs);
    if (!f) rt_fatal("romfs: %s is in the index but cannot be opened", abs);
    plat_mutex_lock(&lock);
    for (unsigned j = 1; j < NH; j++)
        if (!handles[j]) {
            handles[j] = f;
            handle_entry[j] = i + 1;
            *out = ROMFS_HANDLE_BASE + j;
            plat_mutex_unlock(&lock);
            return 0;
        }
    plat_mutex_unlock(&lock);
    rt_fatal("RomFS file handle table full (%d)", NH);
}

static int handle_source(uint64_t h, PlatFile **f, uint64_t *size) {
    if (!romfs_handle(h)) return 0;
    uint64_t j = h - ROMFS_HANDLE_BASE;
    plat_mutex_lock_shared(&lock);
    *f = handles[j];
    uint64_t i = handle_entry[j];
    plat_mutex_unlock_shared(&lock);
    if (!*f || !i) return 0;
    *size = files[i - 1].size;
    return 1;
}

uint32_t romfs_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size) {
    PlatFile *f;
    uint64_t fsize;
    if (!out || offset < 0 || !handle_source(h, &f, &fsize)) return FS_R_INVALID;
    if ((uint64_t)offset > fsize || (!buffer && size)) return FS_R_INVALID;
    uint64_t n = size < fsize - (uint64_t)offset ? size : fsize - (uint64_t)offset, done = 0;
    while (done < n) {
        uint32_t chunk = n - done > READ_CHUNK ? READ_CHUNK : (uint32_t)(n - done);
        int64_t r = plat_file_pread(f, (uint8_t *)buffer + done, chunk, (uint64_t)offset + done);
        if (r <= 0) rt_fatal("RomFS read failure at %#llx", (unsigned long long)(offset + done));
        done += (uint64_t)r;
    }
    *out = done;
    return 0;
}

uint32_t romfs_size(int64_t *out, uint64_t h) {
    PlatFile *f;
    uint64_t size;
    if (!out || !handle_source(h, &f, &size)) return FS_R_INVALID;
    *out = (int64_t)size;
    return 0;
}

void romfs_close(uint64_t h) {
    PlatFile *f;
    uint64_t size;
    if (!handle_source(h, &f, &size)) rt_fatal("RomFS: close of invalid file handle %#llx", (unsigned long long)h);
    uint64_t j = h - ROMFS_HANDLE_BASE;
    plat_mutex_lock(&lock);
    handles[j] = NULL;
    handle_entry[j] = 0;
    plat_mutex_unlock(&lock);
    plat_file_close(f);
}

uint32_t romfs_entry_type(int *out, const char *path) {
    if (!out) return FS_R_INVALID;
    const char *p = romfs_path_relative(mount_name, path);
    if (!p) {   // "<mount>:/" itself is the root directory
        size_t n = strlen(mount_name);
        if (romfs_owns(path) && !strcmp(path + n, ":/")) {
            *out = FS_ENTRY_DIRECTORY;
            return 0;
        }
        return FS_R_PATH_NOT_FOUND;
    }
    if (file_index(p) != UINT64_MAX) {
        *out = FS_ENTRY_FILE;
        return 0;
    }
    char dir[4096];
    if (snprintf(dir, sizeof dir, "%s/", p) >= (int)sizeof dir) return FS_R_INVALID;
    if (dir_exists(dir)) {
        *out = FS_ENTRY_DIRECTORY;
        return 0;
    }
    return FS_R_PATH_NOT_FOUND;
}

// ---- overrides
// nn::Result nn::fs::MountRom(const char *name): the Switch 2 SDK's form, without a cache buffer
static void ov_mount_rom(Cpu *c) { ret_x(c, romfs_mount(arg_p(c, 0))); }
// nn::Result nn::fs::QueryMountRomCacheSize(size_t *out): no cache needed
static void ov_query_cache(Cpu *c) {
    uint64_t *out = arg_p(c, 0);
    if (out) *out = 0;
    ret_x(c, out ? 0 : FS_R_INVALID);
}
static void ov_initialize(Cpu *c) { (void)c; fs_init(); }
// nn::Result nn::fs::EnableGlobalFileDataCache(void *buffer, size_t size): the host reads files directly
static void ov_enable_cache(Cpu *c) { ret_x(c, 0); }

HOST_OVERRIDE("_ZN2nn2fs8MountRomEPKc", ov_mount_rom);
HOST_OVERRIDE("_ZN2nn2fs8MountRomEPKcPvm", ov_mount_rom);
HOST_OVERRIDE("_ZN2nn2fs22QueryMountRomCacheSizeEPm", ov_query_cache);
HOST_OVERRIDE("_ZN2nn2fs24InitializeForApplicationEv", ov_initialize);
HOST_OVERRIDE("_ZN2nn2fs25EnableGlobalFileDataCacheEPvm", ov_enable_cache);
