/* Names for ROM offsets, from the cartridge's own file system (the FNT and FAT the header
 * points to), so that the log can say which file a card read belongs to. Built once from the
 * user's dump; nothing of it is kept in the port. */
#include "nitro/romfs.h"

#include "log.h"
#include "rom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t start, end;
    uint16_t id;
} Span;

static uint8_t *s_fnt;
static uint32_t s_fnt_size;
static char **s_names;   /* by file id: the FNT path, or "ovNNN" for an overlay */
static uint32_t s_ovt_off, s_ovt_size; /* the ARM9 overlay table */
static Span *s_spans;    /* by start offset */
static uint32_t s_count; /* files in the FAT */

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void walk(uint16_t dir, const char *path, int depth)
{
    const uint32_t entry = (dir & 0xfff) * 8u;
    uint32_t off;
    uint16_t id;

    if (depth > 16 || entry + 8 > s_fnt_size)
        return;
    off = le32(s_fnt + entry);
    id = le16(s_fnt + entry + 4);
    while (off < s_fnt_size) {
        const uint8_t len = s_fnt[off++];
        const size_t plen = strlen(path), nlen = len & 0x7f;
        char name[384];
        if (!len || off + nlen > s_fnt_size || plen + 1 + nlen >= sizeof(name))
            break;
        memcpy(name, path, plen);
        name[plen] = '/';
        memcpy(name + plen + 1, s_fnt + off, nlen);
        name[plen + 1 + nlen] = 0;
        off += nlen;
        if (len & 0x80) {
            if (off + 2 > s_fnt_size)
                break;
            walk(le16(s_fnt + off), name, depth + 1);
            off += 2;
        } else {
            if (id < s_count && !s_names[id])
                s_names[id] = strdup(name);
            id++;
        }
    }
}

static int by_start(const void *a, const void *b)
{
    const Span *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}

void kh_romfs_init(void)
{
    const uint8_t *h = rom_header();
    const uint32_t fnt_off = le32(h + 0x40), fnt_size = le32(h + 0x44);
    const uint32_t fat_off = le32(h + 0x48), fat_size = le32(h + 0x4c);
    uint8_t *fat;
    uint32_t i;

    s_count = fat_size / 8;
    fat = malloc(fat_size);
    s_fnt = malloc(fnt_size);
    s_names = calloc(s_count, sizeof(*s_names));
    s_spans = malloc(s_count * sizeof(*s_spans));
    if (!fat || !s_fnt || !s_names || !s_spans ||
        rom_read(fat_off, fat, fat_size) != (int)fat_size ||
        rom_read(fnt_off, s_fnt, fnt_size) != (int)fnt_size) {
        LOG("romfs: cannot read the file tables");
        s_count = 0;
        free(fat);
        return;
    }
    s_fnt_size = fnt_size;
    for (i = 0; i < s_count; i++) {
        s_spans[i].start = le32(fat + i * 8);
        s_spans[i].end = le32(fat + i * 8 + 4);
        s_spans[i].id = (uint16_t)i;
    }
    free(fat);
    qsort(s_spans, s_count, sizeof(*s_spans), by_start);
    walk(0xf000, "", 0);
    free(s_fnt);
    /* overlays: the ARM9 overlay table gives each one's file id (entry +0x18) */
    s_ovt_off = le32(h + 0x50), s_ovt_size = le32(h + 0x54);
    {
        uint8_t e[32];
        for (i = 0; i + 32 <= s_ovt_size; i += 32) {
            char name[16];
            uint32_t file;
            if (rom_read(s_ovt_off + i, e, 32) != 32)
                break;
            file = le32(e + 0x18);
            snprintf(name, sizeof(name), "ov%03u", (unsigned)le32(e));
            if (file < s_count && !s_names[file])
                s_names[file] = strdup(name);
        }
    }
    s_fnt = NULL;
    LOG("romfs: %u files", (unsigned)s_count);
}

const char *kh_romfs_describe(uint32_t off, char *buf, int size)
{
    uint32_t lo = 0, hi = s_count;
    while (lo < hi) { /* the last span starting at or before off */
        uint32_t mid = (lo + hi) / 2;
        if (s_spans[mid].start <= off)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (off - s_ovt_off < s_ovt_size) {
        snprintf(buf, size, "overlay table, ov%03u", (unsigned)((off - s_ovt_off) / 32));
    } else if (lo && off < s_spans[lo - 1].end) {
        const Span *sp = &s_spans[lo - 1];
        if (s_names[sp->id])
            snprintf(buf, size, "%s+%x", s_names[sp->id], (unsigned)(off - sp->start));
        else
            snprintf(buf, size, "file %u+%x", (unsigned)sp->id, (unsigned)(off - sp->start));
    } else {
        snprintf(buf, size, "rom %08x", (unsigned)off);
    }
    return buf;
}

int kh_romfs_find(const char *path, uint32_t *offset, uint32_t *size)
{
    uint32_t i;
    for (i = 0; i < s_count; i++) {
        const Span *sp = &s_spans[i];
        if (s_names[sp->id] && !strcmp(s_names[sp->id], path)) {
            *offset = sp->start;
            *size = sp->end - sp->start;
            return 1;
        }
    }
    return 0;
}
