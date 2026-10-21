"""Host checks; these do not replace compiling/running the CG shaders on Vita."""
import ast
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

SOURCE = (Path(__file__).resolve().parents[1] / "platform/core/video.c").read_text()


class CompositionTests(unittest.TestCase):
    def test_simple_shader_variants(self):
        body = SOURCE.split("static const char s_compose_fs[] =", 1)[1].split("\n\n/* Compiled shaders", 1)[0]
        body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
        shader = "".join(ast.literal_eval(s) for s in re.findall(r'"(?:\\.|[^"\\])*"', body))
        for defines in ("", "#define SIMPLE_SCREEN\n", "#define PANEL\n",
                        "#define CAP\n#define SIMPLE_SCREEN\n"):
            with self.subTest(defines=defines):
                result = subprocess.run(["cc", "-E", "-P", "-x", "c", "-"],
                                        input=defines + shader, text=True,
                                        capture_output=True, check=True).stdout
                self.assertEqual("inside(v2d" in result, defines == "")
                self.assertIn("smooth2d(u2d, v2d, uFilt)", result)
                self.assertEqual("tex2D(uPrev" in result, "#define CAP" in defines)
                if not defines:
                    self.assertLess(result.index("else if (inside(v2d"),
                                    result.index("b.rgb = smooth2d"))

    def test_cutscene_pause_not_restricted_to_battle(self):
        root = Path(__file__).resolve().parents[1]
        game = (root / "platform/hw/game.c").read_text()
        call = re.search(r"video_set_pause_dim\((.*?)\);", game, re.S).group(1)
        self.assertIn("PauseMenu_GetMode() != 0", call)
        self.assertNotIn("kh_overlay_loaded", call)
        self.assertIn("0x04000050", call)
        self.assertIn("0x04000054", call)

    def test_detail_to_simple_clears_placements(self):
        root = Path(__file__).resolve().parents[1]
        program = r'''
#include <assert.h>
#include <string.h>
#include "config.h"
#include "video.h"
#include "hw/gpu3d.h"
#include "hw/fieldhud.h"
KhConfig kh_config;
uint32_t data_ov002_0207f614, data_ov023_0208a784[2];
unsigned char data_0204c240;
static int pieces, holes, moved, placed, flat;
static unsigned hidden;
int kh_overlay_loaded(int id) { return id == 22; }
int kh_is_game_heap(unsigned long a, unsigned long n) { return 0; }
ScreenRect video_screen_rect(int screen) { return (ScreenRect){0, 0, 960, 544}; }
void video_set_panel_place(int i, const ScreenRect *r) { placed = r != 0; }
void kh_gpu3d_set_hud_place(const KhGpu3dHudPlace *p) { flat = p != 0; }
void video_set_field_hud(const VideoPiece *p, int n, const float h[][4], int nh,
                         int m, unsigned off) {
    pieces = n; holes = nh; moved = m; hidden = off;
}
int main(void) {
    static uint32_t lower[256*192];
    kh_config.hud_size = 100;
    kh_config.panel[2][KH_PANEL_SW] = 64;
    kh_config.panel[2][KH_PANEL_SH] = 87;
    assert(!strcmp(config_single_screen_name(0), "OFF"));
    assert(!strcmp(config_single_screen_name(1), "SIMPLE"));
    assert(!strcmp(config_single_screen_name(2), "DETAIL"));
    for (int i = 0; i < 3; ++i) {
        kh_fieldhud_frame(1, 0, 0, lower);
        assert(pieces > 0 && holes > 0 && moved && placed && flat && hidden);
        kh_fieldhud_frame(0, 0, 0, lower);
        assert(!pieces && !holes && !moved && !placed && !flat && !hidden);
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            executable = str(Path(tmp) / "fieldhud-test")
            subprocess.run(["cc", "-I" + str(root / "platform"),
                            "-I" + str(root / "platform/core"), "-x", "c", "-",
                            str(root / "platform/hw/fieldhud.c"), "-o", executable],
                           input=program, text=True, check=True)
            subprocess.run([executable], check=True)

    def test_pause_registers(self):
        fn = re.search(r"void video_set_pause_dim\([^)]*\)\n\{.*?\n\}", SOURCE, re.S).group()
        program = "#include <stdint.h>\n#include <assert.h>\nstatic float s_pause_dim;\n" + fn + """
int main(void) {
    video_set_pause_dim(1, 0xc1, 8); assert(s_pause_dim == 8);
    video_set_pause_dim(0, 0xc1, 8); assert(s_pause_dim == 0);
    video_set_pause_dim(1, 0x81, 8); assert(s_pause_dim == 0);
    video_set_pause_dim(1, 0xc0, 8); assert(s_pause_dim == 0);
    video_set_pause_dim(1, 0xc1, 31); assert(s_pause_dim == 16);
    video_set_pause_dim(1, 0xc1, 0); assert(s_pause_dim == 0);
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            executable = str(Path(tmp) / "pause-test")
            subprocess.run(["cc", "-x", "c", "-", "-o", executable],
                           input=program, text=True, check=True)
            subprocess.run([executable], check=True)


if __name__ == "__main__":
    unittest.main()
