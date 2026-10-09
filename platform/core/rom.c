#include "rom.h"

#include "log.h"
#include "sha1.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <psp2/kernel/processmgr.h>
#include <string.h>

static SceUID s_fd = -1;
static SceUID s_lock = -1;
static uint8_t s_header[0x200];

/* A full SHA-1 over 256 MiB takes a while on the Vita, so a successful check is remembered in a
 * stamp file keyed by the dump's size and modification time. */
static void stamp_key(const SceIoStat *st, char *out, size_t n)
{
    snprintf(out, n, "%s %lld %u-%u-%u %u:%u:%u\n", ROM_EXPECTED_SHA1, (long long)st->st_size,
             st->st_mtime.year, st->st_mtime.month, st->st_mtime.day,
             st->st_mtime.hour, st->st_mtime.minute, st->st_mtime.second);
}

/* A copy of the same dump gets a new modification time: then a fingerprint of 64 slices of
 * 64 KiB spread over the file (4 MiB, under a second) is compared with the one stored at the
 * last full check, and the 44 s SHA-1 is skipped when they agree. */
static void fingerprint(char out[41])
{
    enum { SLICES = 64, SLICE = 64 * 1024 };
    uint8_t *buf = malloc(SLICE), digest[20];
    Sha1 sha;
    int i;
    out[0] = 0;
    if (!buf)
        return;
    sha1_init(&sha);
    for (i = 0; i < SLICES; i++) {
        const uint32_t off = (uint32_t)(((uint64_t)(ROM_EXPECTED_SIZE - SLICE) * i) / (SLICES - 1));
        const int n = sceIoPread(s_fd, buf, SLICE, off);
        if (n > 0)
            sha1_update(&sha, buf, n);
    }
    free(buf);
    sha1_final(&sha, digest);
    for (i = 0; i < 20; i++)
        sprintf(out + i * 2, "%02x", digest[i]);
}

/* the stamp: the key line, then the fingerprint line */
static int stamp_read(const char *stamp_path, char *key, size_t nkey, char fp[41])
{
    char buf[256] = { 0 }, *nl;
    SceUID fd = sceIoOpen(stamp_path, SCE_O_RDONLY, 0);
    key[0] = fp[0] = 0;
    if (fd < 0)
        return 0;
    sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    nl = strchr(buf, '\n');
    if (nl) {
        snprintf(key, nkey, "%.*s", (int)(nl - buf + 1), buf);
        snprintf(fp, 41, "%.40s", nl + 1);
    }
    return 1;
}

static void stamp_write(const char *stamp_path, const char *key, const char *fp)
{
    SceUID fd = sceIoOpen(stamp_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, key, strlen(key));
        sceIoWrite(fd, fp, strlen(fp));
        sceIoWrite(fd, "\n", 1);
        sceIoClose(fd);
    }
}

static int hash_file(void (*progress)(uint32_t, uint32_t), char hex[41])
{
    enum { CHUNK = 1 << 20 };
    uint8_t *buf = malloc(CHUNK), digest[20];
    uint32_t done = 0;
    Sha1 sha;
    int i;

    if (!buf)
        return -1;
    sha1_init(&sha);
    while (done < ROM_EXPECTED_SIZE) {
        int n = sceIoPread(s_fd, buf, CHUNK, done);
        if (n <= 0)
            break;
        sha1_update(&sha, buf, n);
        done += n;
        if (progress)
            progress(done, ROM_EXPECTED_SIZE);
    }
    free(buf);
    sha1_final(&sha, digest);
    for (i = 0; i < 20; i++)
        sprintf(hex + i * 2, "%02x", digest[i]);
    return done == ROM_EXPECTED_SIZE ? 0 : -1;
}

RomStatus rom_open(const char *path, const char *stamp_path,
                   void (*progress)(uint32_t, uint32_t))
{
    SceIoStat st;
    char key[160], hex[41];

    if (sceIoGetstat(path, &st) < 0 || (s_fd = sceIoOpen(path, SCE_O_RDONLY, 0)) < 0) {
        LOG("rom: %s not found", path);
        return ROM_MISSING;
    }
    if ((uint64_t)st.st_size != ROM_EXPECTED_SIZE) {
        LOG("rom: size %lld, expected %u", (long long)st.st_size, ROM_EXPECTED_SIZE);
        return ROM_WRONG_SIZE;
    }
    sceIoPread(s_fd, s_header, sizeof(s_header), 0);
    if (memcmp(s_header + 0x0c, ROM_GAMECODE, 4) != 0) {
        LOG("rom: gamecode %.4s, expected %s", (const char *)s_header + 0x0c, ROM_GAMECODE);
        return ROM_WRONG_GAME;
    }

    stamp_key(&st, key, sizeof(key));
    {
        char old_key[160], old_fp[41], fp[41];
        stamp_read(stamp_path, old_key, sizeof(old_key), old_fp);
        if (strcmp(old_key, key) != 0) {
            fingerprint(fp);
            if (fp[0] && strcmp(fp, old_fp) == 0) {
                LOG("rom: same dump as verified before (fingerprint), SHA-1 skipped");
            } else {
                LOG("rom: verifying SHA-1 (first boot with this dump)");
                if (hash_file(progress, hex) != 0 || strcmp(hex, ROM_EXPECTED_SHA1) != 0) {
                    LOG("rom: SHA-1 %s, expected %s", hex, ROM_EXPECTED_SHA1);
                    return ROM_WRONG_HASH;
                }
            }
            stamp_write(stamp_path, key, fp);
        } else if (!old_fp[0]) {
            /* a stamp from before the fingerprint: add it */
            fingerprint(fp);
            stamp_write(stamp_path, key, fp);
        }
    }
    s_lock = sceKernelCreateMutex("kh_rom", 0, 0, NULL);
    LOG("rom: ok (%.12s %.4s)", (const char *)s_header, (const char *)s_header + 0x0c);
    return ROM_OK;
}

/* ---- read cache -------------------------------------------------------------------------
 * The game reads the cartridge in small pieces (512-byte pages, 128-byte save chunks...), and
 * every sceIoPread on the memory card costs milliseconds whatever its size: loading the title
 * screen's 120 KiB took 1.5 s at ~6 ms a page. Reads are served from aligned 64 KiB blocks kept
 * in a small LRU cache; reads of 128 KiB or more go straight to the file. */

#define BLOCK_SHIFT 16
#define BLOCK_SIZE (1u << BLOCK_SHIFT)
#define BLOCKS 16
#define DIRECT_MIN (128u * 1024u)

typedef struct {
    uint32_t base; /* block offset, or 0xffffffff when empty */
    uint32_t len;  /* valid bytes */
    uint32_t used; /* LRU stamp */
} Block;

static Block s_block[BLOCKS];
static uint8_t s_block_data[BLOCKS][BLOCK_SIZE] __attribute__((aligned(64)));
static uint32_t s_stamp;
static RomStats s_stats;

static int block_get(uint32_t base)
{
    int i, victim = 0;
    for (i = 0; i < BLOCKS; i++) {
        if (s_block[i].base == base) {
            s_block[i].used = ++s_stamp;
            s_stats.hits++;
            return i;
        }
        if (s_block[i].used < s_block[victim].used)
            victim = i;
    }
    {
        uint64_t t0 = sceKernelGetProcessTimeWide();
        int n = sceIoPread(s_fd, s_block_data[victim], BLOCK_SIZE, base);
        s_stats.io_us += (uint32_t)(sceKernelGetProcessTimeWide() - t0);
        s_stats.io_calls++;
        s_block[victim].base = base;
        s_block[victim].len = n > 0 ? (uint32_t)n : 0;
        s_block[victim].used = ++s_stamp;
    }
    return victim;
}

#define PATCHES_MAX 64
static struct {
    uint32_t offset, size;
    uint8_t *data;
} s_patch[PATCHES_MAX];
static int s_npatch;

void rom_patch(uint32_t offset, const void *data, uint32_t size)
{
    uint8_t *copy;
    if (s_npatch >= PATCHES_MAX || !size || !(copy = malloc(size)))
        return;
    memcpy(copy, data, size);
    s_patch[s_npatch].offset = offset;
    s_patch[s_npatch].size = size;
    s_patch[s_npatch].data = copy;
    s_npatch++;
}

/* the patches over [offset, offset + n) applied to what was read there */
static void apply_patches(uint32_t offset, uint8_t *dst, uint32_t n)
{
    int i;
    for (i = 0; i < s_npatch; i++) {
        const uint32_t ps = s_patch[i].offset, pe = ps + s_patch[i].size;
        const uint32_t lo = ps > offset ? ps : offset;
        const uint32_t hi = pe < offset + n ? pe : offset + n;
        if (lo < hi)
            memcpy(dst + (lo - offset), s_patch[i].data + (lo - ps), hi - lo);
    }
}

int rom_read(uint32_t offset, void *dst, uint32_t size)
{
    const uint32_t start = offset;
    int n = 0;
    if (offset >= ROM_EXPECTED_SIZE)
        return 0;
    if (size > ROM_EXPECTED_SIZE - offset)
        size = ROM_EXPECTED_SIZE - offset;
    sceKernelLockMutex(s_lock, 1, NULL);
    s_stats.reads++;
    if (!s_stamp) {
        int i;
        for (i = 0; i < BLOCKS; i++)
            s_block[i].base = 0xffffffffu;
        s_stamp = 1;
    }
    if (size >= DIRECT_MIN) {
        uint64_t t0 = sceKernelGetProcessTimeWide();
        n = sceIoPread(s_fd, dst, size, offset);
        s_stats.io_us += (uint32_t)(sceKernelGetProcessTimeWide() - t0);
        s_stats.io_calls++;
    } else {
        uint8_t *out = dst;
        while (size) {
            const uint32_t base = offset & ~(BLOCK_SIZE - 1), in = offset - base;
            const Block *b = &s_block[block_get(base)];
            uint32_t take = BLOCK_SIZE - in;
            if (take > size)
                take = size;
            if (in >= b->len)
                break;
            if (take > b->len - in)
                take = b->len - in;
            memcpy(out, s_block_data[b - s_block] + in, take);
            out += take;
            offset += take;
            size -= take;
            n += (int)take;
        }
    }
    if (n > 0 && s_npatch)
        apply_patches(start, dst, (uint32_t)n);
    sceKernelUnlockMutex(s_lock, 1);
    return n;
}

void rom_take_stats(RomStats *out)
{
    sceKernelLockMutex(s_lock, 1, NULL);
    *out = s_stats;
    memset(&s_stats, 0, sizeof(s_stats));
    sceKernelUnlockMutex(s_lock, 1);
}

const uint8_t *rom_header(void)
{
    return s_header;
}

void rom_close(void)
{
    if (s_fd >= 0)
        sceIoClose(s_fd);
    s_fd = -1;
}
