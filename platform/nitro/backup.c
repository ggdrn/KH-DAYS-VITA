/* The cartridge's backup memory (the save chip), as the ARM7 serves it to the NitroSDK's CARD
 * library over PXI (tag FS, 11).
 *
 * The ARM9 fills a CARDiCommandArg in its memory, sends the request number (and, for
 * CARD_REQ_INIT, the address of that block) with the error flag set, and sleeps until a reply
 * on the same tag (CARDi_OnFifoRecv). Here each request runs at once on an in-memory image of
 * the chip and the reply is queued as the PXI interrupt.
 *
 * The image is ux0:data/khdays/days.sav, the chip's raw contents: the same format DS
 * emulators use, so an emulator save can be copied in (DeSmuME's .dsv footer is ignored; a
 * shorter file is padded with 0xff, as an erased chip reads). Writes are flushed to the file
 * about a second after the last one, through a temporary file and a rename, so a save cut
 * short by a crash or power loss leaves the previous file whole. */
#include "nitro/backup.h"

#include "log.h"
#include "nitro/arm7.h"
#include "paths.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TAG_FS 11
#define MAX_SIZE (8u << 20) /* the largest backup the SDK knows (64 Mbit flash) */

/* CARDiCommandArg (the SDK's card.h) */
typedef struct {
    int32_t result;
    uint32_t type;
    uint32_t id;
    uint32_t src;
    uint32_t dst;
    uint32_t len;
} CommandArg;

enum {
    REQ_INIT = 0, REQ_ACK, REQ_IDENTIFY, REQ_READ_ID, REQ_READ_ROM, REQ_WRITE_ROM,
    REQ_READ_BACKUP, REQ_WRITE_BACKUP, REQ_PROGRAM_BACKUP, REQ_VERIFY_BACKUP,
    REQ_ERASE_PAGE_BACKUP, REQ_ERASE_SECTOR_BACKUP, REQ_ERASE_CHIP_BACKUP,
    REQ_READ_STATUS, REQ_WRITE_STATUS, REQ_ERASE_SUBSECTOR_BACKUP,
};
enum { RESULT_SUCCESS = 0, RESULT_FAILURE = 1, RESULT_INVALID_PARAM = 2, RESULT_UNSUPPORTED = 3 };

static CommandArg *s_cmd;
static int s_want_cmd;      /* the word after CARD_REQ_INIT is the command block's address */
static uint8_t *s_image;    /* the chip, s_size bytes */
static uint32_t s_size;
static volatile int s_dirty;
static volatile uint64_t s_last_write_us;
static SceUID s_lock = -1; /* the image between the game (requests) and the display (flush) */

static void lock(void)
{
    if (s_lock < 0)
        s_lock = sceKernelCreateMutex("kh_backup", 0, 0, NULL);
    sceKernelLockMutex(s_lock, 1, NULL);
}

static void unlock(void)
{
    sceKernelUnlockMutex(s_lock, 1);
}

static const char DESMUME_FOOTER[] = "|-DESMUME SAVE-|";

/* The file into s_image (s_size bytes), 0xff where it is shorter. */
static void load(void)
{
    SceIoStat st;
    SceUID fd;
    int n = 0;

    memset(s_image, 0xff, s_size);
    if (sceIoGetstat(KH_SAVE_PATH, &st) < 0) {
        LOG("backup: no %s, starting with an erased chip", KH_SAVE_PATH);
        return;
    }
    fd = sceIoOpen(KH_SAVE_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) {
        LOG("backup: cannot open %s (%08x)", KH_SAVE_PATH, fd);
        return;
    }
    n = sceIoRead(fd, s_image, s_size);
    if ((uint64_t)st.st_size > s_size) {
        /* DeSmuME appends a footer ending in "|-DESMUME SAVE-|"; anything else is a mismatch */
        char tail[sizeof(DESMUME_FOOTER) - 1];
        sceIoLseek(fd, -(SceOff)sizeof(tail), SCE_SEEK_END);
        if (sceIoRead(fd, tail, sizeof(tail)) == (int)sizeof(tail) &&
            !memcmp(tail, DESMUME_FOOTER, sizeof(tail)))
            LOG("backup: DeSmuME save, footer ignored");
        else
            LOG("backup: %s is %lld bytes, larger than the chip (%u): only the start is used",
                KH_SAVE_PATH, (long long)st.st_size, (unsigned)s_size);
    }
    sceIoClose(fd);
    LOG("backup: loaded %d of %u bytes from %s", n < 0 ? 0 : n, (unsigned)s_size, KH_SAVE_PATH);
}

static void flush_locked(void)
{
    static const char tmp[] = KH_SAVE_PATH ".tmp";
    SceUID fd;
    int n;

    if (!s_dirty || !s_image)
        return;
    s_dirty = 0;
    fd = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        LOG("backup: cannot write %s (%08x)", tmp, fd);
        s_dirty = 1;
        return;
    }
    n = sceIoWrite(fd, s_image, s_size);
    sceIoClose(fd);
    if (n != (int)s_size) {
        LOG("backup: short write (%d of %u), %s kept as it was", n, (unsigned)s_size, KH_SAVE_PATH);
        s_dirty = 1;
        return;
    }
    sceIoRemove(KH_SAVE_PATH);
    sceIoRename(tmp, KH_SAVE_PATH);
    LOG("backup: saved %s", KH_SAVE_PATH);
}

void kh_backup_flush(void)
{
    lock();
    flush_locked();
    unlock();
}

void kh_backup_tick(void)
{
    if (s_dirty && sceKernelGetProcessTimeWide() - s_last_write_us > 1000000)
        kh_backup_flush();
}

/* CARD_REQ_IDENTIFY: the type the game declares (CARD_IdentifyBackup) sets the chip's size */
static int identify(uint32_t type)
{
    const uint32_t size = type ? 1u << ((type >> 8) & 0xff) : 0;
    if (!type)
        return RESULT_SUCCESS; /* CARD_BACKUP_TYPE_NOT_USE */
    if (size > MAX_SIZE)
        return RESULT_INVALID_PARAM;
    if (s_image && size == s_size)
        return RESULT_SUCCESS;
    flush_locked();
    free(s_image);
    s_image = malloc(size);
    s_size = s_image ? size : 0;
    if (!s_image)
        return RESULT_FAILURE;
    LOG("backup: type %08x, %u bytes", (unsigned)type, (unsigned)size);
    load();
    return RESULT_SUCCESS;
}

static int in_chip(uint32_t off, uint32_t len)
{
    return s_image && off <= s_size && len <= s_size - off;
}

static int run(int req)
{
    CommandArg *c = s_cmd;
    switch (req) {
    case REQ_INIT:
    case REQ_ACK:
    case REQ_READ_ID:
    case REQ_READ_STATUS:
    case REQ_WRITE_STATUS:
        return RESULT_SUCCESS;
    case REQ_IDENTIFY:
        return identify(c->type);
    case REQ_READ_BACKUP:
        if (!in_chip(c->src, c->len))
            return RESULT_INVALID_PARAM;
        memcpy((void *)(uintptr_t)c->dst, s_image + c->src, c->len);
        return RESULT_SUCCESS;
    case REQ_WRITE_BACKUP:
    case REQ_PROGRAM_BACKUP:
        if (!in_chip(c->dst, c->len))
            return RESULT_INVALID_PARAM;
        memcpy(s_image + c->dst, (const void *)(uintptr_t)c->src, c->len);
        s_last_write_us = sceKernelGetProcessTimeWide();
        s_dirty = 1;
        return RESULT_SUCCESS;
    case REQ_VERIFY_BACKUP:
        if (!in_chip(c->dst, c->len))
            return RESULT_INVALID_PARAM;
        return memcmp(s_image + c->dst, (const void *)(uintptr_t)c->src, c->len) ? RESULT_FAILURE
                                                                                  : RESULT_SUCCESS;
    case REQ_ERASE_PAGE_BACKUP:
    case REQ_ERASE_SECTOR_BACKUP:
    case REQ_ERASE_SUBSECTOR_BACKUP:
        if (!in_chip(c->dst, c->len))
            return RESULT_INVALID_PARAM;
        memset(s_image + c->dst, 0xff, c->len);
        s_last_write_us = sceKernelGetProcessTimeWide();
        s_dirty = 1;
        return RESULT_SUCCESS;
    case REQ_ERASE_CHIP_BACKUP:
        if (!s_image)
            return RESULT_INVALID_PARAM;
        memset(s_image, 0xff, s_size);
        s_last_write_us = sceKernelGetProcessTimeWide();
        s_dirty = 1;
        return RESULT_SUCCESS;
    default:
        return RESULT_UNSUPPORTED;
    }
}

void kh_backup_pxi(uint32_t data)
{
    static int logged;
    int result;

    if (s_want_cmd) {
        s_want_cmd = 0;
        s_cmd = (CommandArg *)(uintptr_t)data;
        s_cmd->result = RESULT_SUCCESS;
        kh_arm7_reply(TAG_FS, REQ_INIT, 1);
        return;
    }
    if (data == REQ_INIT) {
        s_want_cmd = 1; /* answered once the address arrives */
        return;
    }
    if (!s_cmd) {
        LOG("backup: request %u before CARD_REQ_INIT", (unsigned)data);
        return;
    }
    lock();
    result = run((int)data);
    unlock();
    if (logged++ < 64 || result != RESULT_SUCCESS)
        LOGV("backup: request %u src %08x dst %08x len %x -> %d", (unsigned)data,
            (unsigned)s_cmd->src, (unsigned)s_cmd->dst, (unsigned)s_cmd->len, result);
    s_cmd->result = result;
    kh_arm7_reply(TAG_FS, data, 1);
}
