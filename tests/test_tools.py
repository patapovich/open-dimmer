#!/usr/bin/env python3
"""Host tests of the image tools: tools/mkimage.py (version byte rules),
tools/embed_ztu.py (what the Zigbee module may carry) and tools/gen_curves.py
(curve tables), tools/changelog.py (release notes), tools/check_app.py (header flags and build
of each build kind, the watchdog first in main()). Synthetic images and disassembly, no
toolchain needed. Run: python3 tests/test_tools.py"""
import contextlib, importlib.util, io, os, re, struct, subprocess, sys, tempfile, unittest, zlib

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools")
MAGIC, HDR = 0x5A445041, 0xC0
SIM, MEASURE, DIAG = 0x01, 0x02, 0x04


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
        for v, flags, msg in ((0x8E, DIAG, "DIAGNOSTICS BUILD: takes no update (back over SWD)"),
                              (0x8F, MEASURE, "MEASUREMENT BUILD: outputs off, takes no update")):
            rc, out, made = self.mkimage(v, flags)
            self.assertEqual(rc, 0, out)
            self.assertTrue(made)
            self.assertIn(msg, out)
            self.assertNotIn("warning", out)
        rc, out, _ = self.mkimage(0x9B)
        self.assertNotIn("takes no update", out)

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
                              (0x8E, DIAG, "refusing a diagnostics build"),
                              (0x9B, DIAG, "refusing a diagnostics build"),
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

def load_check_app():
    sys.dont_write_bytecode = True                # no __pycache__ in tools/
    spec = importlib.util.spec_from_file_location("check_app", os.path.join(TOOLS, "check_app.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# main() as arm-none-eabi-objdump -d --disassemble=main shows it (normal build, trimmed): the
# reset flags, the watchdog (LSI wait, keys, PR, RLR, SR wait, reload), health_init, the clock.
MAIN_OK = """
08002c20 <main>:
 8002c20:\tb5f0      \tpush\t{r4, r5, r6, r7, lr}
 8002c22:\t4c86      \tldr\tr4, [pc, #536]\t@ (8002e3c <main+0x21c>)
 8002c26:\t6e23      \tldr\tr3, [r4, #96]\t@ 0x60
 8002c32:\t6623      \tstr\tr3, [r4, #96]\t@ 0x60
 8002c4c:\t4f7d      \tldr\tr7, [pc, #500]\t@ (8002e44 <main+0x224>)
 8002c4e:\t4b7e      \tldr\tr3, [pc, #504]\t@ (8002e48 <main+0x228>)
 8002c50:\t603b      \tstr\tr3, [r7, #0]
 8002c52:\t4b7e      \tldr\tr3, [pc, #504]\t@ (8002e4c <main+0x22c>)
 8002c54:\t603b      \tstr\tr3, [r7, #0]
 8002c6e:\t4b78      \tldr\tr3, [pc, #480]\t@ (8002e50 <main+0x230>)
 8002c70:\t603b      \tstr\tr3, [r7, #0]
 8002c76:\tf7ff fee7 \tbl\t8002a48 <health_init>
 8002c84:\t4b73      \tldr\tr3, [pc, #460]\t@ (8002e54 <main+0x234>)
 8002c9c:\tf000 ff5c \tbl\t8003b58 <SystemCoreClockUpdate>
 8002d80:\tf000 f922 \tbl\t8002fc8 <phase_init>
 8002e3c:\t40021000 \t.word\t0x40021000
 8002e44:\t40003000 \t.word\t0x40003000
 8002e48:\t0000cccc \t.word\t0x0000cccc
 8002e4c:\t00005555 \t.word\t0x00005555
 8002e50:\t0000aaaa \t.word\t0x0000aaaa
 8002e54:\t1fff0f10 \t.word\t0x1fff0f10
""".replace("\\t", "\t")


def header_img(flags, build):
    d = bytearray(finished_img(0x9C, flags))
    struct.pack_into("<H", d, HDR + 0x0E, build)
    return bytes(d)


class CheckApp(unittest.TestCase):
    """tools/check_app.py: the header of each build kind and the watchdog first in main()."""

    def test_header_of_each_build_kind(self):
        ca = load_check_app()
        for kind, flags, build in (("normal", 0x08, 300), ("diag", 0x0C, 300),
                                   ("measure", 0x0A, 300), ("sim", 0x09, 0xFFFE)):
            self.assertEqual(ca.check_header(header_img(flags, build), kind, 300), [], kind)
        self.assertEqual(ca.check_header(header_img(0x08, 307), "normal", 307), [])

    def test_header_refusals(self):
        ca = load_check_app()
        for kind, flags, build, msg in (
                ("normal", 0x00, 300, "flags 0x00"),          # no CONFIRMS (a 2.1.x header)
                ("normal", 0x18, 300, "flags 0x18"),          # RING_HIGH before release M
                ("normal", 0x28, 300, "flags 0x28"),          # a reserved flag
                ("normal", 0x0C, 300, "flags 0x0C"),          # a bench flag in a normal build
                ("diag", 0x04, 300, "flags 0x04"),
                ("sim", 0x09, 300, "build 300"),              # SIM carries 0xFFFE
                ("normal", 0x08, 0, "build 0"),
                ("normal", 0x08, 301, "build 301"),
                ("normal", 0x08, 0x2C01, "build 11265")):    # 300 written big-endian
            errors = ca.check_header(header_img(flags, build), kind, 300)
            self.assertTrue(any(msg in e for e in errors), (kind, flags, build, errors))
        self.assertEqual(ca.check_header(b"\0" * 0x200, "normal", 300), ["no app header at 0xC0"])

    def test_release_build_is_the_literal_in_tuya_h(self):
        self.assertEqual(load_check_app().release_build() % 100, 0)

    @staticmethod
    def move(text, line_with, before_line_with):
        """The disassembly with the line containing line_with moved up before another."""
        lines = text.splitlines()
        moved = next(ln for ln in lines if line_with in ln)
        lines.remove(moved)
        lines.insert(next(i for i, ln in enumerate(lines) if before_line_with in ln), moved)
        return "\n".join(lines)

    def test_watchdog_first_in_main(self):
        ca = load_check_app()
        self.assertEqual(ca.check_order(MAIN_OK), [])
        # the clock set up before the watchdog (the calibration read moved up)
        self.assertEqual(ca.check_order(self.move(MAIN_OK, " 8002c84:", " 8002c4c:")),
                         ["main() sets up the clock before it starts the watchdog"])
        # a call before the watchdog (health_init moved up)
        self.assertEqual(ca.check_order(self.move(MAIN_OK, "<health_init>", " 8002c4c:")),
                         ["main() calls health_init before it starts the watchdog"])
        # clock_init not inlined and called first
        not_inlined = MAIN_OK.replace(" 8002c26:", " 8002c24:\tf000 f800 \tbl\t8003000 <clock_init>\n"
                                                   " 8002c26:")
        self.assertEqual(ca.check_order(not_inlined),
                         ["main() sets up the clock before it starts the watchdog",
                          "main() calls clock_init before it starts the watchdog"])
        # no watchdog at all
        no_key = MAIN_OK.replace("0x0000cccc", "0x0000cccd")
        self.assertIn("never starts the watchdog", ca.check_order(no_key)[0])
        # watchdog_start not inlined, called first: fine
        called = no_key.replace(" 8002c4c:", " 8002c4a:\tf000 f800 \tbl\t8003000 <watchdog_start>\n"
                                             " 8002c4c:")
        self.assertEqual(ca.check_order(called), [])
        self.assertEqual(ca.check_order(""), ["no main() in the disassembly"])


class Changelog(unittest.TestCase):
    def load(self):
        spec = importlib.util.spec_from_file_location("changelog", os.path.join(TOOLS, "changelog.py"))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        return mod

    def test_section_of_a_version(self):
        cl = self.load()
        text = "# Changelog\n\n## 2.1.12 (2026-11-18)\n\n- a\n- b\n\n## 2.1.1 (x)\n\n- c\n"
        self.assertEqual(cl.section(text, "v2.1.12"), "- a\n- b")
        self.assertEqual(cl.section(text, "2.1.1"), "- c")        # not a prefix match of 2.1.12
        self.assertEqual(cl.section(text, "2.1.13"), "")
        wrapped = "## 1.0.0 (x)\n\n- one\n  two\n- three\n"
        self.assertEqual(cl.section(wrapped, "1.0.0"), "- one two\n- three")

    def test_every_tagged_version_has_notes(self):
        """The repo's own CHANGELOG.md has a section for the version in src/tuya.h."""
        with open(os.path.join(TOOLS, "..", "src", "tuya.h")) as f:
            vb = int(re.search(r"#define FW_VERSION\s+(0x[0-9A-Fa-f]+)\s+/\*", f.read().split("#ifndef FW_VERSION", 1)[1]).group(1), 16)
        version = f"{vb >> 6}.{(vb >> 4) & 3}.{vb & 15}"
        r = subprocess.run([sys.executable, os.path.join(TOOLS, "changelog.py"), version],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(r.stdout.strip().startswith("- "))


if __name__ == "__main__":
    unittest.main(verbosity=1)
