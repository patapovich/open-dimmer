#!/usr/bin/env python3
"""Host tests of the image tools: tools/mkimage.py (version byte rules),
tools/embed_ztu.py (what the Zigbee module may carry) and tools/gen_curves.py
(curve tables). Synthetic images, no toolchain needed. Run: python3 tests/test_tools.py"""
import contextlib, importlib.util, io, os, struct, subprocess, sys, tempfile, unittest, zlib

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools")
MAGIC, HDR = 0x5A445041, 0xC0
SIM, MEASURE = 0x01, 0x02


def app_bin(version, flags=0):
    """A raw app as the linker gives it: vectors for 0x08001000, header without size/CRC."""
    d = bytearray(b"\xa5" * 0x200)
    struct.pack_into("<II", d, 0, 0x20000C00, 0x08001101)
    struct.pack_into("<IIIBB", d, HDR, MAGIC, 0, 0, version, flags)
    return bytes(d)


def finished_img(version, flags=0):
    """A finished image made without mkimage's rules (what a bypass would give)."""
    d = bytearray(app_bin(version, flags))
    struct.pack_into("<II", d, HDR + 4, len(d), 0)
    struct.pack_into("<I", d, HDR + 8, zlib.crc32(bytes(d)) & 0xFFFFFFFF)
    return bytes(d)


class Tools(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, tool, data, *extra):
        src = os.path.join(self.dir, "in.bin")
        out = os.path.join(self.dir, "out")
        if os.path.exists(out):
            os.remove(out)
        with open(src, "wb") as f:
            f.write(data)
        p = subprocess.run([sys.executable, os.path.join(TOOLS, tool), src, out, *extra],
                           capture_output=True, text=True)
        return p.returncode, p.stdout + p.stderr, os.path.exists(out)

    def mkimage(self, version, flags=0):
        return self.run_tool("mkimage.py", app_bin(version, flags))

    def embed(self, img):
        return self.run_tool("embed_ztu.py", img)

    # --- mkimage: version byte rules --------------------------------------------------
    def test_mkimage_normal_version(self):
        rc, out, made = self.mkimage(0x9B)
        self.assertEqual(rc, 0, out)
        self.assertTrue(made)
        self.assertIn("version 2.1.11 (0x9B)", out)
        self.assertNotIn("warning", out)

    def test_mkimage_refuses_ff(self):
        for flags in (0, SIM, MEASURE):
            rc, out, made = self.mkimage(0xFF, flags)
            self.assertNotEqual(rc, 0, out)
            self.assertIn("version byte 0xFF", out)
            self.assertFalse(made, "a refused image must not leave an output file")

    def test_mkimage_bf_only_for_sim(self):
        for flags in (0, MEASURE):
            rc, out, made = self.mkimage(0xBF, flags)
            self.assertNotEqual(rc, 0, out)
            self.assertIn("reserved for the SIM bench build", out)
            self.assertFalse(made)
        rc, out, made = self.mkimage(0xBF, SIM)
        self.assertEqual(rc, 0, out)
        self.assertTrue(made)
        self.assertIn("SIM BENCH BUILD", out)
        self.assertNotIn("warning", out)

    def test_mkimage_warns_when_codes_run_out(self):
        rc, out, _ = self.mkimage(0xAE)          # 0xAF..0xBE: 16 left
        self.assertEqual(rc, 0, out)
        self.assertNotIn("warning", out)
        rc, out, made = self.mkimage(0xAF)       # 15 left before 0xBF
        self.assertEqual(rc, 0, out)
        self.assertTrue(made)
        self.assertIn("leaves 15 version codes before 0xBF", out)
        rc, out, _ = self.mkimage(0xBE)
        self.assertIn("leaves 0 version codes before 0xBF", out)
        rc, out, _ = self.mkimage(0xC0)          # 3.0.0: far from 0xFF
        self.assertEqual(rc, 0, out)
        self.assertNotIn("warning", out)
        rc, out, _ = self.mkimage(0xF0)
        self.assertEqual(rc, 0, out)
        self.assertIn("leaves 14 version codes before 0xFF", out)

    def test_mkimage_diag_and_measure_versions(self):
        for v, flags in ((0x8E, 0), (0x8F, MEASURE)):
            rc, out, made = self.mkimage(v, flags)
            self.assertEqual(rc, 0, out)
            self.assertTrue(made)
            self.assertNotIn("warning", out)

    # --- embed_ztu: what the module may carry -----------------------------------------
    def test_embed_accepts_normal(self):
        rc, out, made = self.embed(finished_img(0x9B))
        self.assertEqual(rc, 0, out)
        self.assertTrue(made)
        with open(os.path.join(self.dir, "out")) as f:
            self.assertIn("version 2.1.11", f.read())

    def test_embed_refusals(self):
        for v, flags, msg in ((0xBF, SIM, "refusing a SIM bench build"),
                              (0x9B, SIM, "refusing a SIM bench build"),
                              (0xBF, 0, "refusing a SIM bench build"),
                              (0x8F, MEASURE, "refusing a measurement build"),
                              (0xFF, 0, "refusing version 0xFF")):
            rc, out, made = self.embed(finished_img(v, flags))
            self.assertNotEqual(rc, 0, f"0x{v:02X}/{flags}: {out}")
            self.assertIn(msg, out)
            self.assertFalse(made)


def load_gen_curves():
    sys.dont_write_bytecode = True                # no __pycache__ in tools/
    spec = importlib.util.spec_from_file_location("gen_curves", os.path.join(TOOLS, "gen_curves.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class Curves(unittest.TestCase):
    def test_generator_gives_src_curves_h(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            load_gen_curves().main()
        with open(os.path.join(TOOLS, "..", "src", "curves.h")) as f:
            self.assertEqual(out.getvalue(), f.read())

    def test_generator_refuses_a_falling_table(self):
        gen = load_gen_curves()
        self.assertIsNone(gen.falling([0, 5, 5, 9]))
        self.assertEqual(gen.falling([0, 5, 4, 9]), 2)
        # a curve that dips in the middle (dim_curve's unsigned maths would wrap)
        gen.CURVES = gen.CURVES + [("dip", lambda lv: lv / 1000.0 - (0.05 if 400 < lv < 500 else 0))]
        out = io.StringIO()
        with contextlib.redirect_stdout(out), self.assertRaises(SystemExit) as e:
            gen.main()
        self.assertIn("curve dip falls at point", str(e.exception.code))
        self.assertEqual(out.getvalue(), "")       # nothing half-written to curves.h

if __name__ == "__main__":
    unittest.main(verbosity=1)
