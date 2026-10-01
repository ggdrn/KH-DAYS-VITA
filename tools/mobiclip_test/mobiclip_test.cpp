// Host test for the portable MobiClip decoder the port uses in place of ov024's ARM payload
// (libs/mobiclip/video/portable in the decomp): decodes the first frames of a video from the
// user's dump the way ov024 drives it (six rotating plane buffers, the state's two run/level
// tables) and writes them as greyscale-plus-colour PNGs for a look. The decomp's own
// tools/tests/mobiclip_frame_core_test.cpp compares the same decoder with FFmpeg byte for byte.
//
//     tools/mobiclip_test/run.sh [/mv/802.mods] [frames]   -> build/mobiclip_test/frame_NNN.png
#include "mobiclip_frame_core.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "nitro/romfs.h"
int rom_read(uint32_t offset, void *dst, uint32_t size);
const uint8_t *rom_header(void);
extern unsigned char data_ov024_0208a7c4[];
extern unsigned char data_ov024_020886c4[];
}

static FILE *g_rom;
static uint8_t g_header[0x200];
extern "C" int rom_read(uint32_t o, void *d, uint32_t n)
{
    std::fseek(g_rom, o, SEEK_SET);
    return (int)std::fread(d, 1, n, g_rom);
}
extern "C" const uint8_t *rom_header(void) { return g_header; }

static unsigned readU32(const std::vector<unsigned char> &b, unsigned at)
{
    return b[at] | b[at + 1] << 8 | b[at + 2] << 16 | (unsigned)b[at + 3] << 24;
}

// YCoCg (MobiClip's planes) to RGB, as ov024's converter does it, without its dither
static void writePpm(const char *path, const unsigned char *luma, const unsigned char *chroma,
                     unsigned w, unsigned h)
{
    FILE *f = std::fopen(path, "wb");
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
            int Y = luma[y * MOBICLIP_PLANE_STRIDE + x];
            const unsigned char *row = chroma + (y / 2) * MOBICLIP_PLANE_STRIDE;
            int Co = row[x / 2] - 128, Cg = row[MOBICLIP_PLANE_STRIDE / 2 + x / 2] - 128;
            int t = Y - Cg, rgb[3] = { t + Co, Y + Cg, t - Co };
            for (int c = 0; c < 3; ++c)
                std::fputc(rgb[c] < 0 ? 0 : rgb[c] > 255 ? 255 : rgb[c], f);
        }
    std::fclose(f);
}

int main(int argc, char **argv)
{
    const std::string path = argc > 2 ? argv[2] : "/mv/802.mods";
    const unsigned frames = argc > 3 ? (unsigned)std::atoi(argv[3]) : 60;
    g_rom = std::fopen(argv[1], "rb");
    if (!g_rom || std::fread(g_header, 1, 0x200, g_rom) != 0x200)
        return std::fprintf(stderr, "cannot read %s\n", argv[1]), 1;
    kh_romfs_init();
    uint32_t off, size;
    if (!kh_romfs_find(path.c_str(), &off, &size))
        return std::fprintf(stderr, "%s not in the ROM\n", path.c_str()), 1;
    std::vector<unsigned char> mods(size + 64, 0);
    rom_read(off, mods.data(), size);

    const unsigned w = readU32(mods, 12), h = readU32(mods, 16);
    std::printf("%s: %ux%u, %c%c\n", path.c_str(), w, h, mods[4], mods[5]);
    unsigned packet = 0x30;
    if (mods[4] == 'N' && mods[5] == '3')
        for (;;) {
            const bool last = mods[packet] == 'H' && mods[packet + 1] == 'E';
            packet += 4 + (mods[packet + 2] | (mods[packet + 3] << 8)) * 4;
            if (last)
                break;
        }

    std::vector<unsigned char> lumaRing[6], chromaRing[6];
    unsigned char *luma[6], *chroma[6];
    for (unsigned i = 0; i < 6; ++i) {
        lumaRing[i].assign(MOBICLIP_PLANE_STRIDE * h, 0);
        chromaRing[i].assign(MOBICLIP_PLANE_STRIDE * h / 2, 0);
        luma[i] = lumaRing[i].data();
        chroma[i] = chromaRing[i].data();
    }
    MobiClipDecoderState state;
    std::memset(&state, 0, sizeof state);
    state.nWidth = w;
    state.nHeight = h;
    state.nQuantizer = 12;
    state.apCoefficientTables[0] = data_ov024_0208a7c4;
    state.apCoefficientTables[1] = data_ov024_020886c4;

    unsigned decoded = 0;
    for (unsigned frame = 0; frame < frames && packet + 4 <= size; ++frame) {
        const unsigned psize = readU32(mods, packet) >> 14;
        unsigned char *ol = luma[5], *oc = chroma[5];
        for (unsigned i = 5; i > 0; --i)
            luma[i] = luma[i - 1], chroma[i] = chroma[i - 1];
        luma[0] = ol, chroma[0] = oc;
        for (unsigned i = 0; i < 6; ++i)
            state.apLuma[i] = luma[i], state.apChroma[i] = chroma[i];
        state.pBitstream = mods.data() + packet + 4;
        const int used = MobiClip_DecodeFrameCore(&state);
        if (used <= 0 || (unsigned)used > psize + 2) {
            std::printf("frame %u: decoder returned %d for a %u-byte packet\n", frame, used, psize);
            return 1;
        }
        if (frame % 200 == 0 || frame + 1 == frames) {
            char name[64];
            std::snprintf(name, sizeof name, "frame_%03u.ppm", frame);
            writePpm(name, luma[0], chroma[0], w, h);
        }
        decoded++;
        packet += 4 + psize;
    }
    std::printf("decoded %u frames, quantizer now %u\n", decoded, state.nQuantizer);
    return 0;
}
