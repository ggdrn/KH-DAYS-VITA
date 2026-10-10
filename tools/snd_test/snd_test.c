/* Host test for platform/audio/snd7.c: plays sequences from the ROM's sound archive
 * (/snd/sound_data.sdat) through the driver as the game would set them up, and writes WAVs.
 *
 *     tools/snd_test/run.sh ../days.nds [seq numbers...]   -> build/snd_test/seq_N.wav
 *
 * The ROM is read locally; nothing of it is written anywhere but the build directory. */
#include "audio/snd7.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *s_rom;
static long s_rom_size;
static uint8_t *s_arena;
static uint32_t s_top = 0x100; /* 0 is NULL */

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static uint32_t alloc(uint32_t n)
{
    uint32_t a = (s_top + 31) & ~31u;
    s_top = a + n;
    return a;
}

/* NitroFS: the file id of a path */
static int find_file(const char *path)
{
    const uint32_t fnt = rd32(s_rom + 0x40);
    int dir = 0;
    char part[128];
    while (*path == '/') path++;
    while (*path) {
        const char *sl = strchr(path, '/');
        size_t n = sl ? (size_t)(sl - path) : strlen(path);
        const uint8_t *e = s_rom + fnt + rd32(s_rom + fnt + dir * 8);
        int id = rd16(s_rom + fnt + dir * 8 + 4), found = -1;
        memcpy(part, path, n); part[n] = 0;
        while (*e) {
            int len = *e & 0x7f, isdir = *e & 0x80;
            if ((size_t)len == n && !memcmp(e + 1, part, n)) {
                found = isdir ? (rd16(e + 1 + len) & 0xfff) : id;
                if (!isdir && sl) return -1;
                break;
            }
            e += 1 + len + (isdir ? 2 : 0);
            if (!isdir) id++;
        }
        if (found < 0) return -1;
        if (!sl) return found;
        dir = found;
        path = sl + 1;
    }
    return -1;
}

static void wav(const char *name, const int16_t *s, int frames)
{
    FILE *f = fopen(name, "wb");
    uint8_t h[44] = "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x02\0\0\0\0\0\0\0\0\0\x04\0\x10\0data";
    wr32(h + 4, 36 + frames * 4);
    wr32(h + 24, (uint32_t)snd7_rate);
    wr32(h + 28, (uint32_t)snd7_rate * 4);
    wr32(h + 40, frames * 4);
    fwrite(h, 1, 44, f);
    fwrite(s, 4, frames, f);
    fclose(f);
}

static uint32_t s_alarms;
static void to_arm9(uint32_t w) { (void)w; s_alarms++; }

int main(int argc, char **argv)
{
    FILE *f;
    int id, i;
    uint32_t sdat, fat, info, seqrec, bankrec, warec, work;
    if (argc < 2) { fprintf(stderr, "usage: snd_test rom.nds [seq...]\n"); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); s_rom_size = ftell(f); fseek(f, 0, SEEK_SET);
    s_rom = malloc(s_rom_size);
    fread(s_rom, 1, s_rom_size, f);
    fclose(f);
    id = find_file("snd/sound_data.sdat");
    if (id < 0) { fprintf(stderr, "no sound_data.sdat\n"); return 1; }
    {
        const uint32_t fatoff = rd32(s_rom + 0x48);
        uint32_t start = rd32(s_rom + fatoff + id * 8), end = rd32(s_rom + fatoff + id * 8 + 4);
        s_arena = calloc(1, 64u << 20);
        sdat = alloc(end - start);
        memcpy(s_arena + sdat, s_rom + start, end - start);
        printf("sound_data.sdat: %u bytes\n", end - start);
    }
    snd7_ptr_base = (uintptr_t)s_arena;
    if (getenv("SND7_RATE"))
        snd7_rate = atoi(getenv("SND7_RATE"));
    snd7_send_to_arm9 = to_arm9;
    {
        const uint8_t *S = s_arena + sdat;
        info = sdat + rd32(S + 0x18);
        fat = sdat + rd32(S + 0x20);
        seqrec = info + rd32(s_arena + info + 8 + 0 * 4);
        bankrec = info + rd32(s_arena + info + 8 + 2 * 4);
        warec = info + rd32(s_arena + info + 8 + 3 * 4);
        printf("%u sequences, %u banks, %u wave archives\n", rd32(s_arena + seqrec),
               rd32(s_arena + bankrec), rd32(s_arena + warec));
    }
    work = alloc(0x280);
    snd7_set_running(1);

    for (i = 2; i < argc; i++) {
        const int seq = atoi(argv[i]);
        const uint32_t nseq = rd32(s_arena + seqrec);
        uint32_t sinfo, bfile, sfile, binfo, cmd;
        uint16_t bank;
        int k, frames = snd7_rate * 12;
        int16_t *buf;
        double sum = 0;
        Snd7Stats st;
        if ((uint32_t)seq >= nseq || !rd32(s_arena + seqrec + 4 + seq * 4)) { printf("seq %d: none\n", seq); continue; }
        sinfo = info + rd32(s_arena + seqrec + 4 + seq * 4);
        sfile = sdat + rd32(s_arena + fat + 12 + rd16(s_arena + sinfo) * 16);
        bank = rd16(s_arena + sinfo + 4);
        binfo = info + rd32(s_arena + bankrec + 4 + bank * 4);
        bfile = sdat + rd32(s_arena + fat + 12 + rd16(s_arena + binfo) * 16);
        /* link the bank's wave archives, as SND_AssignWaveArc does */
        for (k = 0; k < 4; k++) {
            uint16_t wa = rd16(s_arena + binfo + 4 + k * 2);
            uint32_t wfile = 0;
            if (wa != 0xffff) {
                uint32_t winfo = info + rd32(s_arena + warec + 4 + wa * 4);
                wfile = sdat + rd32(s_arena + fat + 12 + (rd16(s_arena + winfo) & 0xffff) * 16);
            }
            wr32(s_arena + bfile + 0x18 + k * 8, wfile);
        }
        /* SHARED_WORK, START_SEQ(player 0, data, 0, bank) */
        cmd = alloc(48);
        wr32(s_arena + cmd, cmd + 24);
        wr32(s_arena + cmd + 4, 29);
        wr32(s_arena + cmd + 8, work);
        wr32(s_arena + cmd + 24, 0);
        wr32(s_arena + cmd + 28, 0);
        wr32(s_arena + cmd + 32, 0);
        wr32(s_arena + cmd + 36, sfile + rd32(s_arena + sfile + 0x18));
        wr32(s_arena + cmd + 40, 0);
        wr32(s_arena + cmd + 44, bfile);
        snd7_pxi(cmd);
        buf = calloc(frames, 4);
        snd7_render(buf, frames);
        for (k = 0; k < frames * 2; k++)
            sum += (double)buf[k] * buf[k];
        snd7_take_stats(&st);
        printf("seq %d (bank %u): %u notes, %u lists, rms %.0f, peak voices %u, unknown seq ops %u, finished tag %u\n",
               seq, bank, st.notes, st.lists, sqrt(sum / (frames * 2)), st.voices_max, st.unknown_seq,
               rd32(s_arena + work));
        {
            char name[64];
            snprintf(name, sizeof(name), "seq_%d.wav", seq);
            wav(name, buf, frames);
        }
        free(buf);
        /* stop it before the next */
        cmd = alloc(24);
        wr32(s_arena + cmd, 0); wr32(s_arena + cmd + 4, 1); wr32(s_arena + cmd + 8, 0);
        snd7_pxi(cmd);
    }
    return 0;
}
