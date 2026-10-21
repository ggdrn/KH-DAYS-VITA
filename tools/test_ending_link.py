"""Regression: reject the exact wrong-overlay pointer from the 0.9.0 crash."""
import contextlib
import io
import struct
import tempfile
import unittest
from pathlib import Path

from check_elf import check_ending_scene


class EndingLinkTest(unittest.TestCase):
    def check_image(self, wrong_overlay=False, wrong_ctor=False):
        image = bytearray(512)
        image[:6] = b"\x7fELF\x01\x01"
        struct.pack_into("<I", image, 28, 52)
        struct.pack_into("<HH", image, 42, 32, 1)
        struct.pack_into("<IIIII", image, 52, 1, 0, 0x1000, 0, len(image))
        struct.pack_into("<II", image, 128 + 9 * 8, 9, 0x1180 if wrong_overlay else 0x1100)
        struct.pack_into("<IIIII", image, 256, 0x000e0008,
                         0x1300 if wrong_ctor else 0x1200, 0x1240, 4, 0)
        syms = ("00001080 D gSceneTable\n00001100 D data_ov009_02056220\n"
                "00001200 T Ov009_ClassCtor\n00001240 T Ov009_SaveSceneTeardown\n")
        with tempfile.TemporaryDirectory() as tmp:
            elf = Path(tmp) / "ending.elf"
            elf.write_bytes(image)
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                return check_ending_scene(elf, syms)

    def test_correct_scene(self):
        self.assertEqual(self.check_image(), 0)

    def test_wrong_overlay_rejected(self):
        self.assertEqual(self.check_image(wrong_overlay=True), 1)

    def test_wrong_constructor_rejected(self):
        self.assertEqual(self.check_image(wrong_ctor=True), 1)


if __name__ == "__main__":
    unittest.main()
