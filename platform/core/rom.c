#include "rom.h"

#include "log.h"
#include "sha1.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <stdlib.h>
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

static int stamp_matches(const char *stamp_path, const char *key)
{
    char buf[160] = { 0 };
    SceUID fd = sceIoOpen(stamp_path, SCE_O_RDONLY, 0);
    if (fd < 0)
        return 0;
    sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    return strcmp(buf, key) == 0;
}

static void stamp_write(const char *stamp_path, const char *key)
{
    SceUID fd = sceIoOpen(stamp_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, key, strlen(key));
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
    if (!stamp_matches(stamp_path, key)) {
        LOG("rom: verifying SHA-1 (first boot with this dump)");
        if (hash_file(progress, hex) != 0 || strcmp(hex, ROM_EXPECTED_SHA1) != 0) {
            LOG("rom: SHA-1 %s, expected %s", hex, ROM_EXPECTED_SHA1);
            return ROM_WRONG_HASH;
        }
        stamp_write(stamp_path, key);
    }
    s_lock = sceKernelCreateMutex("kh_rom", 0, 0, NULL);
    LOG("rom: ok (%.12s %.4s)", (const char *)s_header, (const char *)s_header + 0x0c);
    return ROM_OK;
}

int rom_read(uint32_t offset, void *dst, uint32_t size)
{
    int n;
    if (offset >= ROM_EXPECTED_SIZE)
        return 0;
    if (size > ROM_EXPECTED_SIZE - offset)
        size = ROM_EXPECTED_SIZE - offset;
    /* sceIoPread carries its own offset, the lock only serialises the card like the DS did */
    sceKernelLockMutex(s_lock, 1, NULL);
    n = sceIoPread(s_fd, dst, size, offset);
    sceKernelUnlockMutex(s_lock, 1);
    return n;
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
