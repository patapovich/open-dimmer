#!/usr/bin/env python3
"""Host tests of the tools: tools/fwimage.py (shared image rules), tools/mkimage.py (the image
rules of docs/BOOTLOADER.md sections 1, 2 and 11), tools/embed_ztu.py (what the Zigbee module
may carry), tools/release/release_check.py (release rules), tools/check_elf.py (CI ELF checks),
tools/swd/install.sh and make flash (against a fake pyOCD / OpenOCD / curl that model the
chip's option bytes, flash and mailbox), tools/ota_swd_emu.py (against a model of the
bootloader's SWD interface), tools/gen_curves.py (curve tables) and tools/changelog.py
(release notes). Synthetic images, no toolchain and no hardware. Run: python3 tests/test_tools.py"""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

sys.dont_write_bytecode = True                    # no __pycache__ in tools/
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, ".."))
TOOLS = os.path.join(REPO, "tools")
MAGIC, HDR = 0x5A445041, 0xC0
SIM, MEASURE, DIAG, CONFIRMS, RING_HIGH = 0x01, 0x02, 0x04, 0x08, 0x10
SP, RESET = 0x20000BE0, 0x08001101
KB = 1024


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


fw = load("fwimage", os.path.join(TOOLS, "fwimage.py"))


def app_bin(version=0x9D, flags=CONFIRMS, build=300, size=0x200, sp=SP, reset=RESET):
    """A raw app as the linker gives it: vectors for 0x08001000, header without size/CRC."""
    d = bytearray(b"\xa5" * size)
    struct.pack_into("<II", d, 0, sp, reset)
    struct.pack_into("<IIIBBH", d, HDR, MAGIC, 0, 0, version, flags, build)
    return bytes(d)


def finish(d):
    """Size and CRC filled in without mkimage's rules (what a bypass would give)."""
    d = bytearray(d)
    struct.pack_into("<II", d, HDR + 4, len(d), 0)
    struct.pack_into("<I", d, HDR + 8, zlib.crc32(bytes(d)) & 0xFFFFFFFF)
    return bytes(d)


def finished_img(version=0x9D, flags=CONFIRMS, build=300, **kw):
    return finish(app_bin(version, flags, build, **kw))


def legacy_img(version=0x9C, flags=0, **kw):
    """A 2.1.x image: no build, no CONFIRMS."""
    return finished_img(version, flags, 0, **kw)


def boot_v2(size=3252, features=0x0F, app_max=0x3800, mailbox=0x20000BE0, version=2):
    d = bytearray(b"\x5a" * size)
    struct.pack_into("<II", d, 0, 0x20000BE0, 0x08000101)
    struct.pack_into("<IBBHII", d, 0x10, 0x4C42444F, version, features, 0xFFFF, app_max, mailbox)
    return bytes(d)


def boot_v1(size=2136):
    """The v1 bootloader has code at 0x10 (80 22 03 4b), no boot_id."""
    d = bytearray(b"\x3c" * size)
    struct.pack_into("<II", d, 0, 0x20000BE0, 0x08000101)
    d[0x10:0x14] = bytes.fromhex("8022034b")
    return bytes(d)


def full_img(boot, img):
    return boot + b"\xff" * (4096 - len(boot)) + img


def crc_forced_zero(d):
    """d (size filled in, CRC field 0) with its last 4 bytes changed so that its CRC is 0:
    CRC-32 is affine in the data, so 32 bit flips span every value (Gaussian elimination)."""
    d = bytearray(d)
    base = len(d) - 4
    d[base:] = bytes(4)
    c0 = zlib.crc32(bytes(d))
    rows = []
    for k in range(32):
        e = bytearray(d)
        e[base + k // 8] ^= 1 << (k % 8)
        rows.append((zlib.crc32(bytes(e)) ^ c0, 1 << k))
    pivots = {}
    for v, m in rows:
        for bit in range(31, -1, -1):
            if not v >> bit & 1:
                continue
            if bit in pivots:
                pv, pm = pivots[bit]
                v, m = v ^ pv, m ^ pm
            else:
                pivots[bit] = (v, m)
                break
    want, sel = c0, 0
    for bit in range(31, -1, -1):
        if want >> bit & 1:
            pv, pm = pivots[bit]
            want, sel = want ^ pv, sel ^ pm
    assert want == 0
    struct.pack_into("<I", d, base, sel ^ struct.unpack_from("<I", d, base)[0])
    assert zlib.crc32(bytes(d)) == 0
    return bytes(d)


class ToolRun(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def path(self, name, data=None):
        p = os.path.join(self.dir, name)
        if data is not None:
            with open(p, "wb") as f:
                f.write(data)
        return p

    def run_py(self, tool, *args, opt=False):
        cmd = [sys.executable] + (["-O"] if opt else []) + [os.path.join(TOOLS, tool), *args]
        p = subprocess.run(cmd, capture_output=True, text=True)
        return p.returncode, p.stdout + p.stderr

    def run_tool(self, tool, data, *extra, opt=False):
        src = self.path("in.bin", data)
        out = self.path("out")
        if os.path.exists(out):
            os.remove(out)
        rc, text = self.run_py(tool, src, out, *extra, opt=opt)
        return rc, text, os.path.exists(out)

    def read(self, name, mode="rb"):
        with open(self.path(name), mode) as f:
            return f.read()


# --- fwimage: the numbers are those of src/ota_proto.h ------------------------------------------
class SharedNumbers(unittest.TestCase):
    def test_constants_match_ota_proto_h(self):
        with open(os.path.join(REPO, "src", "ota_proto.h")) as f:
            text = f.read()
        defs = {}
        for name, value in re.findall(r"^#define\s+(\w+)\s+(0x[0-9A-Fa-f]+|\d+)u?\b", text, re.M):
            defs[name] = int(value, 0)
        pairs = {"BOOT_SIZE": fw.BOOT_SIZE, "APP_BASE": fw.APP_BASE, "APP_HEADER_OFFSET": fw.APP_HEADER_OFFSET,
                 "BOOT_APP_MAX": fw.BOOT_APP_MAX, "APP_LIMIT_LEGACY": fw.APP_LIMIT_LEGACY,
                 "FLASH_TOP": fw.BOOT_BASE + fw.FLASH_SIZE, "APP_MAGIC": fw.APP_MAGIC,
                 "BOOT_ID_MAGIC": fw.BOOT_ID_MAGIC, "BOOT_VERSION": fw.BOOT_VERSION,
                 "APP_FLAG_SIM": fw.FLAG_SIM, "APP_FLAG_MEASURE": fw.FLAG_MEASURE, "APP_FLAG_DIAG": fw.FLAG_DIAG,
                 "APP_FLAGS_BENCH": fw.FLAGS_BENCH, "APP_FLAG_CONFIRMS": fw.FLAG_CONFIRMS,
                 "APP_FLAG_RING_HIGH": fw.FLAG_RING_HIGH, "APP_FLAGS_RESERVED": fw.FLAGS_RESERVED,
                 "OTA_VERSION_OURS_MIN": fw.VERSION_OURS_MIN, "OTA_VERSION_SIM": fw.VERSION_SIM,
                 "OTA_VERSION_NEVER": fw.VERSION_NEVER, "OTA_BUILD_NONE": fw.BUILD_NONE,
                 "OTA_BUILD_MIN": fw.BUILD_MIN, "OTA_BUILD_MAX": fw.BUILD_MAX, "OTA_BUILD_SIM": fw.BUILD_SIM,
                 "OTA_BUILD_NEVER": fw.BUILD_NEVER, "OTA_BUILD_FIRST": fw.BUILD_FIRST,
                 "OTA_BUILD_STEP": fw.BUILD_STEP, "OTA_MAILBOX_RAM": fw.MAILBOX_RAM,
                 "BOOT_FEAT_TRIES": 0x01, "BOOT_FEAT_PROBE": 0x02, "BOOT_FEAT_BUTTON": 0x04,
                 "BOOT_FEAT_LINK": 0x08}
        for name, value in pairs.items():
            self.assertIn(name, defs, name)
            self.assertEqual(defs[name], value, name)
        self.assertEqual(re.search(r"#define\s+BOOT_ID_ADDR\s+\(BOOT_BASE \+ 0x10u\)", text) is not None, True)
        self.assertEqual(fw.BOOT_ID_OFFSET, 0x10)
        self.assertEqual(dict(fw.BOOT_FEAT_NAMES), {0x01: "tries", 0x02: "probe", 0x04: "button", 0x08: "link"})

    def test_boot_sector_crc_is_the_padded_sector(self):
        b = boot_v1()
        self.assertEqual(fw.boot_sector_crc(b), zlib.crc32(b + b"\xff" * (4096 - len(b))))
        self.assertEqual(fw.boot_sector_crc(b + b"\xff" * 100), fw.boot_sector_crc(b))
        with self.assertRaises(fw.Refused):
            fw.boot_sector_crc(b"\0" * 4097)

    def test_boot_id(self):
        self.assertIsNone(fw.boot_id(boot_v1()))
        self.assertEqual(fw.boot_id(boot_v2()), {"version": 2, "features": 0x0F, "app_max": 0x3800,
                                                 "mailbox": 0x20000BE0})
        self.assertIsNone(fw.boot_id(b"\xff" * 0x18))
        self.assertIn("mailbox", fw.boot_id_problem(fw.boot_id(boot_v2(mailbox=0x20000C00))))
        self.assertIn("version", fw.boot_id_problem(fw.boot_id(boot_v2(version=1))))
        self.assertIn("app_max", fw.boot_id_problem(fw.boot_id(boot_v2(app_max=0x4000))))
        self.assertIsNone(fw.boot_id_problem(fw.boot_id(boot_v2())))


# --- mkimage: an image it creates -------------------------------------------------------------
class MkImage(ToolRun):
    def mkimage(self, version=0x9D, flags=CONFIRMS, build=300, *extra, **kw):
        return self.run_tool("mkimage.py", app_bin(version, flags, build, **kw), *extra)

    def assert_refused(self, result, msg):
        rc, out, made = result
        self.assertNotEqual(rc, 0, out)
        self.assertIn(msg, out)
        self.assertFalse(made, "a refused image must not leave an output file")

    def test_normal_image(self):
        rc, out, made = self.mkimage(0x9D, CONFIRMS, 300)
        self.assertEqual(rc, 0, out)
        self.assertIn("version 2.1.13 (0x9D), build 300, flags 0x08 (CONFIRMS)", out)
        self.assertNotIn("warning", out)
        img = self.read("out")
        h = fw.header(img)
        self.assertEqual((h.size, h.build), (len(img), 300))
        self.assertEqual(fw.image_crc(img), h.crc)
        self.assertIn(f"crc {h.crc:08x}", out)

    def test_pads_to_a_word(self):
        rc, out, _ = self.run_tool("mkimage.py", app_bin(size=0x201))
        self.assertEqual(rc, 0, out)
        self.assertEqual(len(self.read("out")), 0x204)

    # build rules
    def test_refuses_build_0_for_an_image_it_creates(self):
        self.assert_refused(self.mkimage(0x9D, CONFIRMS, 0), "without a build number")
        self.assert_refused(self.mkimage(0x9C, 0, 0), "without a build number")

    def test_refuses_builds_1_to_255_and_ffff(self):
        for b in (1, 99, 255):
            self.assert_refused(self.mkimage(0x9D, CONFIRMS, b), "1..255 are never used")
        self.assert_refused(self.mkimage(0x9D, CONFIRMS, 0xFFFF), "0xFFFF is never used")

    def test_fffe_only_for_sim_and_sim_only_fffe(self):
        self.assert_refused(self.mkimage(0x9D, CONFIRMS, 0xFFFE), "reserved for SIM builds")
        self.assert_refused(self.mkimage(0xBF, SIM | CONFIRMS, 300), "a SIM build must have build 0xFFFE")
        rc, out, made = self.mkimage(0xBF, SIM | CONFIRMS, 0xFFFE)
        self.assertEqual(rc, 0, out)
        self.assertTrue(made)
        self.assertIn("SIM BENCH BUILD", out)
        self.assertIn("build 0xFFFE (SIM)", out)

    def test_refuses_a_build_without_confirms(self):
        for flags in (0, DIAG, MEASURE, RING_HIGH):
            self.assert_refused(self.mkimage(0x9D, flags, 300), "without the CONFIRMS flag")
        self.assert_refused(self.mkimage(0xBF, SIM, 0xFFFE), "without the CONFIRMS flag")

    def test_release_and_test_builds(self):
        rc, out, _ = self.mkimage(0x9D, CONFIRMS, 400)
        self.assertEqual(rc, 0, out)
        self.assert_refused(self.mkimage(0x9D, CONFIRMS, 305), "must be a release build")
        rc, out, made = self.mkimage(0x9D, CONFIRMS, 305, "--test", "5")
        self.assertEqual(rc, 0, out)
        self.assertTrue(made)
        self.assert_refused(self.mkimage(0x9D, CONFIRMS, 305, "--test", "6"), "--test 6 wants the release build + 6")
        self.assert_refused(self.mkimage(0x9D, CONFIRMS, 300, "--test", "5"), "--test 5")
        for bad in ("0", "100", "x"):
            self.assert_refused(self.mkimage(0x9D, CONFIRMS, 305, "--test", bad), "a test build takes 1..99")
        rc, out, _ = self.mkimage(0x8E, DIAG | CONFIRMS, 307, "--test", "7")
        self.assertEqual(rc, 0, out)
        self.assertIn("DIAGNOSTICS BUILD", out)

    def test_refuses_crc_0(self):
        d = bytearray(app_bin(size=0x200))
        struct.pack_into("<I", d, HDR + 4, len(d))
        d = bytearray(crc_forced_zero(bytes(d)))
        struct.pack_into("<I", d, HDR + 4, 0)                 # as the linker gives it
        self.assert_refused(self.run_tool("mkimage.py", bytes(d)), "image CRC 0")

    def test_size_limits(self):
        self.assert_refused(self.mkimage(size=12 * KB + 4), "only with RING_HIGH")
        rc, out, _ = self.mkimage(0x9D, CONFIRMS | RING_HIGH, 300, size=13 * KB)
        self.assertEqual(rc, 0, out)
        rc, out, _ = self.mkimage(size=12 * KB)
        self.assertEqual(rc, 0, out)
        self.assert_refused(self.mkimage(0x9D, CONFIRMS | RING_HIGH, 300, size=14 * KB + 4), "max 14336")
        rc, out, _ = self.mkimage(0x9D, CONFIRMS | RING_HIGH, 300, size=14 * KB)
        self.assertEqual(rc, 0, out)

    def test_refuses_reserved_flags(self):
        for f in (0x20, 0x40, 0x80):
            self.assert_refused(self.mkimage(0x9D, CONFIRMS | f, 300), "0x20..0x80 are reserved")

    def test_refuses_vectors_the_bootloader_would_not_start(self):
        self.assert_refused(self.mkimage(sp=0x30000BE0), "not in RAM")
        self.assert_refused(self.mkimage(reset=0x08000101), "outside the image")
        self.assert_refused(self.mkimage(reset=0x08001201), "outside the image")

    def test_refuses_a_wrong_magic(self):
        d = bytearray(app_bin())
        d[HDR] ^= 1
        self.assert_refused(self.run_tool("mkimage.py", bytes(d)), "no app header")

    # version byte rules (kept from 2.1.x)
    def test_refuses_ff(self):
        self.assert_refused(self.mkimage(0xFF, CONFIRMS, 300), "version byte 0xFF")
        self.assert_refused(self.mkimage(0xFF, SIM | CONFIRMS, 0xFFFE), "version byte 0xFF")
        self.assert_refused(self.mkimage(0xFF, MEASURE | CONFIRMS, 300), "version byte 0xFF")

    def test_bf_only_for_sim(self):
        for flags in (CONFIRMS, MEASURE | CONFIRMS):
            self.assert_refused(self.mkimage(0xBF, flags, 300), "reserved for the SIM bench build")

    def test_refuses_a_stock_major(self):
        self.assert_refused(self.mkimage(0x47, CONFIRMS, 300), "major below 2")
        self.assert_refused(self.mkimage(0x7F, CONFIRMS, 300), "major below 2")

    def test_warns_when_codes_run_out(self):
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

    def test_diag_and_measure_versions(self):
        for v, flags, msg in ((0x8E, DIAG | CONFIRMS, "DIAGNOSTICS BUILD: takes no update (back over SWD)"),
                              (0x8F, MEASURE | CONFIRMS, "MEASUREMENT BUILD: outputs off, takes no update")):
            rc, out, made = self.mkimage(v, flags, 300)
            self.assertEqual(rc, 0, out)
            self.assertTrue(made)
            self.assertIn(msg, out)
            self.assertNotIn("warning", out)
        rc, out, _ = self.mkimage(0x9D)
        self.assertNotIn("takes no update", out)

    def test_bad_usage(self):
        rc, out, _ = self.run_tool("mkimage.py", app_bin(), "--bogus")
        self.assertNotEqual(rc, 0)
        self.assertIn("unknown option", out)


# --- mkimage --full: the whole flash for an SWD install -----------------------------------------
class MkImageFull(ToolRun):
    def full(self, img, boot, *extra):
        app = self.path("app.img", img)
        b = self.path("boot.bin", boot)
        out = self.path("full.bin")
        if os.path.exists(out):
            os.remove(out)
        rc, text = self.run_py("mkimage.py", app, "--full", b, out, *extra)
        return rc, text, os.path.exists(out)

    def assert_refused(self, result, msg):
        rc, out, made = result
        self.assertNotEqual(rc, 0, out)
        self.assertIn(msg, out)
        self.assertFalse(made)

    def test_v2_boot_and_app(self):
        img, boot = finished_img(), boot_v2()
        rc, out, made = self.full(img, boot)
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.read("full.bin"), full_img(boot, img))
        self.assertIn(f"boot sector CRC-32 0x{fw.boot_sector_crc(boot):08X} (DP 136)", out)
        self.assertIn("bootloader: version 2 (features 0x0F tries+probe+button+link", out)

    def test_boot_without_boot_id_only_with_old_boot(self):
        img = finished_img()
        self.assert_refused(self.full(img, boot_v1()), "without boot_id at 0x10")
        rc, out, made = self.full(img, boot_v1(), "--old-boot")
        self.assertEqual(rc, 0, out)
        self.assertIn("v1 or older", out)
        self.assert_refused(self.full(img, boot_v2(), "--old-boot"), "--old-boot, but the bootloader has a boot_id")
        self.assert_refused(self.full(finished_img(0x9D, CONFIRMS | RING_HIGH, 300, size=13 * KB), boot_v1(),
                                      "--old-boot"), "the bootloader takes 12288")

    def test_legacy_app_only_with_legacy_app(self):
        """Bench test B6: the 2.1.12 app under the v2 bootloader."""
        old = legacy_img()
        self.assert_refused(self.full(old, boot_v2()), "--legacy-app")
        rc, out, made = self.full(old, boot_v2(), "--legacy-app")
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.read("full.bin"), full_img(boot_v2(), old))
        self.assert_refused(self.full(finished_img(), boot_v2(), "--legacy-app"), "--legacy-app takes a 2.1.x image")
        self.assert_refused(self.full(legacy_img(0x9C, CONFIRMS), boot_v2(), "--legacy-app"),
                            "--legacy-app takes a 2.1.x image")

    def test_refusals(self):
        self.assert_refused(self.full(finished_img(), b"\0" * 4097), "bootloader is 4097 bytes")
        self.assert_refused(self.full(finished_img(), boot_v2(mailbox=0x20000C00)), "bootloader identity")
        self.assert_refused(self.full(finished_img(), b"\xff" * 64, "--old-boot"), "not a bootloader for sector 0")
        self.assert_refused(self.full(finished_img(0x9D, CONFIRMS | RING_HIGH, 300, size=13 * KB),
                                      boot_v2(app_max=0x3000)), "the bootloader takes 12288")
        bad = bytearray(finished_img())
        bad[0x150] ^= 1
        self.assert_refused(self.full(bytes(bad), boot_v2()), "CRC mismatch")
        self.assert_refused(self.full(app_bin(), boot_v2()), "not an app image made by tools/mkimage.py")
        self.assert_refused(self.full(finished_img(0x9D, CONFIRMS, 0xFFFF), boot_v2()), "0xFFFF")
        # a test build (made with --test) goes into a full image as it is
        rc, out, _ = self.full(finished_img(0x9D, CONFIRMS, 305), boot_v2())
        self.assertEqual(rc, 0, out)
        # bench builds are SWD images: they go into a full image
        rc, out, _ = self.full(finished_img(0xBF, SIM | CONFIRMS, 0xFFFE), boot_v2())
        self.assertEqual(rc, 0, out)


# --- embed_ztu: what the module may carry ----------------------------------------------------
class EmbedZtu(ToolRun):
    def embed(self, img, *extra, opt=False):
        return self.run_tool("embed_ztu.py", img, *extra, opt=opt)

    def assert_refused(self, result, msg):
        rc, out, made = result
        self.assertNotEqual(rc, 0, out)
        self.assertIn(msg, out)
        self.assertFalse(made)

    def test_accepts_a_normal_v2_image(self):
        img = finished_img()
        rc, out, made = self.embed(img)
        self.assertEqual(rc, 0, out)
        self.assertIn("version 2.1.13, build 300", out)
        c = self.read("out", "r")
        # py32-bump (fork workflow) reads this exact format
        self.assertRegex(c, r"own MCU firmware: version 2\.1\.13\n")
        self.assertIn("Image build: 300; header flags 0x08.", c)
        rows = re.findall(r"^    (0x[0-9a-f]{2}(?:, 0x[0-9a-f]{2})*),?$", c, re.M)
        self.assertEqual(bytes(int(x, 16) for r in rows for x in r.split(", ")), img)

    def test_bench_and_version_refusals(self):
        for v, flags, build, msg in ((0xBF, SIM | CONFIRMS, 0xFFFE, "refusing a SIM bench build"),
                                     (0x9D, SIM | CONFIRMS, 300, "refusing a SIM bench build"),
                                     (0xBF, CONFIRMS, 300, "refusing a SIM bench build"),
                                     (0x8F, MEASURE | CONFIRMS, 300, "refusing a measurement build"),
                                     (0x8E, DIAG | CONFIRMS, 307, "refusing a diagnostics build"),
                                     (0x9D, DIAG | CONFIRMS, 300, "refusing a diagnostics build"),
                                     (0xFF, CONFIRMS, 300, "refusing version 0xFF"),
                                     (0x47, CONFIRMS, 300, "major below 2")):
            self.assert_refused(self.embed(finished_img(v, flags, build)), msg)

    def test_build_rules(self):
        for b, msg in ((1, "1..255"), (255, "1..255"), (0xFFFE, "0xFFFE"), (0xFFFF, "0xFFFF")):
            self.assert_refused(self.embed(finished_img(0x9D, CONFIRMS, b)), msg)
        self.assert_refused(self.embed(finished_img(0x9D, 0, 300)), "without the CONFIRMS flag")
        rc, out, _ = self.embed(finished_img(0x9D, CONFIRMS, 0xFFFD))
        self.assertEqual(rc, 0, out)
        rc, out, _ = self.embed(finished_img(0x9D, CONFIRMS, 256))
        self.assertEqual(rc, 0, out)

    def test_build_0_only_as_a_legacy_rollback(self):
        self.assert_refused(self.embed(legacy_img()), "only as a rollback with --legacy")
        rc, out, made = self.embed(legacy_img(), "--legacy")
        self.assertEqual(rc, 0, out)
        self.assertIn("Image build: none (2.1.x rollback)", self.read("out", "r"))
        self.assert_refused(self.embed(legacy_img(0x9D), "--legacy"), "up to 0x9C")
        self.assert_refused(self.embed(legacy_img(0x9C, CONFIRMS), "--legacy"), "neither CONFIRMS nor RING_HIGH")
        self.assert_refused(self.embed(legacy_img(0x9C, RING_HIGH), "--legacy"), "neither CONFIRMS nor RING_HIGH")
        self.assert_refused(self.embed(finished_img(), "--legacy"), "--legacy is for 2.1.x images")

    def test_image_info_vectors(self):
        """The phase-3 ZTU's image_info: SP exactly 0x20000BE0, an odd reset vector in the image."""
        self.assert_refused(self.embed(finished_img(sp=0x20000C00)), "not 0x20000BE0")
        self.assert_refused(self.embed(finished_img(reset=0x08001100)), "is even")
        self.assert_refused(self.embed(finished_img(reset=0x08001201)), "outside the image")
        self.assert_refused(self.embed(finished_img(reset=0x08000F01)), "outside the image")

    def test_layout(self):
        self.assert_refused(self.embed(finished_img(size=12 * KB + 4)), "only with RING_HIGH")
        rc, out, _ = self.embed(finished_img(0x9D, CONFIRMS | RING_HIGH, 300, size=13 * KB))
        self.assertEqual(rc, 0, out)
        self.assert_refused(self.embed(finished_img(0x9D, CONFIRMS | RING_HIGH, 300, size=14 * KB + 4)), "max 14336")
        for f in (0x20, 0x80):
            self.assert_refused(self.embed(finished_img(0x9D, CONFIRMS | f, 300)), "reserved")

    def test_finished_image_checks(self):
        bad = bytearray(finished_img())
        bad[0x150] ^= 1
        self.assert_refused(self.embed(bytes(bad)), "CRC mismatch")
        self.assert_refused(self.embed(app_bin()), "not an app image made by tools/mkimage.py")
        self.assert_refused(self.embed(finished_img() + b"\xff" * 4), "not an app image made by tools/mkimage.py")
        self.assert_refused(self.embed(b"\0" * 0x40), "shorter than its header")
        d = bytearray(app_bin())
        struct.pack_into("<I", d, HDR + 4, len(d))
        self.assert_refused(self.embed(crc_forced_zero(bytes(d))), "image CRC 0")

    def test_checks_survive_python_O(self):
        """Every check is a sys.exit (item 42): python3 -O does not skip one."""
        with open(os.path.join(TOOLS, "embed_ztu.py")) as f:
            self.assertNotRegex(f.read(), r"^\s*assert\b")
        with open(os.path.join(TOOLS, "fwimage.py")) as f:
            self.assertNotRegex(f.read(), r"^\s*assert\b")
        self.assert_refused(self.embed(finished_img(sp=0x20000C00), opt=True), "not 0x20000BE0")
        bad = bytearray(finished_img())
        bad[0x150] ^= 1
        self.assert_refused(self.embed(bytes(bad), opt=True), "CRC mismatch")


# --- fwimage full / notes: install.sh's image check and the release notes ------------------------
class FullImageCheck(ToolRun):
    def check(self, full, *opts):
        return self.run_py("fwimage.py", "full", self.path("f.bin", full), *opts)

    def test_v2_image_shows_boot_id_and_crc(self):
        boot = boot_v2()
        rc, out = self.check(full_img(boot, finished_img()))
        self.assertEqual(rc, 0, out)
        self.assertIn("bootloader: version 2 (features 0x0F", out)
        self.assertIn(f"boot sector CRC-32: 0x{fw.boot_sector_crc(boot):08X} (DP 136", out)
        self.assertIn("build 300", out)

    def test_old_boot(self):
        f = full_img(boot_v1(), finished_img())
        rc, out = self.check(f)
        self.assertNotEqual(rc, 0)
        self.assertIn("OLD_BOOT=1", out)
        rc, out = self.check(f, "--old-boot")
        self.assertEqual(rc, 0, out)
        self.assertIn("v1 or older", out)

    def test_over_12_kb_erases_the_settings(self):
        f = full_img(boot_v2(), finished_img(0x9D, CONFIRMS | RING_HIGH, 300, size=13 * KB))
        rc, out = self.check(f)
        self.assertNotEqual(rc, 0)
        self.assertIn("ERASES THE SETTINGS RING", out)
        rc, out = self.check(f, "--erase-settings")
        self.assertEqual(rc, 0, out)
        rc, out = self.check(full_img(boot_v2(), finished_img(size=12 * KB)))
        self.assertEqual(rc, 0, out)
        rc, out = self.check(full_img(boot_v1(), finished_img(0x9D, CONFIRMS | RING_HIGH, 300, size=13 * KB)),
                             "--old-boot", "--erase-settings")
        self.assertNotEqual(rc, 0)
        self.assertIn("without boot_id (12 KB max)", out)

    def test_refusals(self):
        boot = boot_v2()
        rc, out = self.check(full_img(boot, finished_img(0xBF, SIM | CONFIRMS, 0xFFFE)))
        self.assertIn("SIM bench build", out)
        self.assertNotEqual(rc, 0)
        bad = bytearray(full_img(boot, finished_img()))
        bad[0x1150] ^= 1
        rc, out = self.check(bytes(bad))
        self.assertIn("app CRC mismatch", out)
        rc, out = self.check(finished_img())
        self.assertIn("not a full image", out)
        rc, out = self.check(full_img(boot_v2(app_max=0x3000), finished_img(0x9D, CONFIRMS | RING_HIGH, 300,
                                                                            size=13 * KB)), "--erase-settings")
        self.assertIn("the bootloader takes 12288", out)
        rc, out = self.check(full_img(boot, finished_img()) + b"\0")
        self.assertIn("not 0xFF", out)
        rc, out = self.check(full_img(b"\xff" * 64, finished_img()), "--old-boot")
        self.assertNotEqual(rc, 0)
        self.assertIn("not a bootloader for sector 0", out)

    def test_refusal_still_shows_the_bootloader(self):
        rc, out = self.check(full_img(boot_v1(), finished_img()))
        self.assertNotEqual(rc, 0)
        self.assertIn(f"boot sector CRC-32: 0x{fw.boot_sector_crc(boot_v1()):08X}", out)
        self.assertIn("bootloader: v1 or older", out)

    def test_measure_and_diag_noted(self):
        rc, out = self.check(full_img(boot_v2(), finished_img(0x8F, MEASURE | CONFIRMS, 300)))
        self.assertEqual(rc, 0, out)
        self.assertIn("MEASUREMENT build", out)

    def test_release_notes_line(self):
        boot = boot_v2()
        rc, out = self.run_py("fwimage.py", "notes", self.path("b.bin", boot))
        self.assertEqual(rc, 0, out)
        self.assertIn(f"{len(boot)} of 4096 bytes", out)
        self.assertIn(f"0x{fw.boot_sector_crc(boot):08X}", out)
        f = self.path("f.bin", full_img(boot, finished_img()))
        rc, out = self.run_py("fwimage.py", "notes", self.path("b.bin"), f)
        self.assertEqual(rc, 0, out)
        rc, out = self.run_py("fwimage.py", "notes", self.path("b.bin"), self.path("g.bin", full_img(boot_v1(), finished_img())))
        self.assertNotEqual(rc, 0)
        self.assertIn("not boot.bin padded", out)
        rc, out = self.run_py("fwimage.py", "notes", self.path("v1.bin", boot_v1()))
        self.assertIn("v1 or older", out)


# --- release rules ------------------------------------------------------------------------------
rc_mod = load("release_check", os.path.join(TOOLS, "release", "release_check.py"))


def tuya_h(build=None, sim=True):
    t = "#ifndef FW_VERSION\n#define FW_VERSION 0x9D   /* x */\n#endif\n"
    if build is not None:
        t += (("#if SIM_ZC\n#define FW_BUILD 0xFFFE\n#else\n" if sim else "") + f"#define FW_BUILD {build}u\n"
              + ("#endif\n" if sim else ""))
    return t


class ReleaseRules(unittest.TestCase):
    def check(self, image, tag, head, tags, prev_h, images=None):
        return rc_mod.check(image, tag, head, tags, lambda t: prev_h.get(t), lambda t: (images or {}).get(t))

    def test_fw_build_parse(self):
        self.assertEqual(rc_mod.fw_build_of(tuya_h()), 0)
        self.assertEqual(rc_mod.fw_build_of(None), 0)
        self.assertEqual(rc_mod.fw_build_of(tuya_h(300)), 300)
        self.assertEqual(rc_mod.fw_build_of(tuya_h(400, sim=False)), 400)
        self.assertEqual(rc_mod.fw_build_of("#define FW_BUILD 0x12C\n"), 300)
        self.assertEqual(rc_mod.fw_build_of("#define FW_BUILD_IMAGE 7\n"), 0)
        with self.assertRaises(rc_mod.Refused):
            rc_mod.fw_build_of("#define FW_BUILD 300\n#define FW_BUILD 400\n")

    def test_tag_byte(self):
        self.assertEqual(rc_mod.tag_byte("v2.1.12"), 0x9C)
        self.assertEqual(rc_mod.tag_byte("v2.1.13"), 0x9D)
        self.assertIsNone(rc_mod.tag_byte("v2.1.16"))
        self.assertIsNone(rc_mod.tag_byte("py32-v2"))

    def test_first_release_is_300(self):
        tags = {"v2.1.11": 0x9B, "v2.1.12": 0x9C}
        prev = {"v2.1.11": tuya_h(), "v2.1.12": tuya_h()}
        lines = self.check(finished_img(0x9D, CONFIRMS, 300), "v2.1.13", tuya_h(300), tags, prev)
        self.assertIn("build 300 (previous release v2.1.12: build none)", lines[0])
        with self.assertRaisesRegex(rc_mod.Refused, "must be 300"):
            self.check(finished_img(0x9D, CONFIRMS, 400), "v2.1.13", tuya_h(400), tags, prev)

    def test_no_tags_at_all(self):
        lines = self.check(finished_img(0x9D, CONFIRMS, 300), "v2.1.13", tuya_h(300), {}, {})
        self.assertIn("previous release none", lines[0])

    def test_previous_plus_100(self):
        tags = {"v2.1.12": 0x9C, "v2.1.13": 0x9D}
        prev = {"v2.1.12": tuya_h(), "v2.1.13": tuya_h(300)}
        self.check(finished_img(0x9E, CONFIRMS, 400), "v2.1.14", tuya_h(400), tags, prev)
        for b in (500, 350, 300, 399):
            with self.assertRaisesRegex(rc_mod.Refused, "must be 400"):
                self.check(finished_img(0x9E, CONFIRMS, b), "v2.1.14", tuya_h(b), tags, prev)

    def test_image_build_is_fw_build(self):
        tags, prev = {"v2.1.12": 0x9C}, {"v2.1.12": tuya_h()}
        with self.assertRaisesRegex(rc_mod.Refused, "FW_TEST or an override"):
            self.check(finished_img(0x9D, CONFIRMS, 305), "v2.1.13", tuya_h(300), tags, prev)
        with self.assertRaisesRegex(rc_mod.Refused, "has no FW_BUILD"):
            self.check(finished_img(0x9D, CONFIRMS, 300), "v2.1.13", tuya_h(), tags, prev)

    def test_version_byte_above_every_other_tag(self):
        tags = {"v2.1.12": 0x9C, "v2.1.13": 0x9D, "v2.1.14": 0x9E}
        prev = {t: tuya_h(300) for t in tags}
        with self.assertRaisesRegex(rc_mod.Refused, "not above the release tag.*v2.1.14"):
            self.check(finished_img(0x9D, CONFIRMS, 400), "v2.1.13", tuya_h(400), tags, prev)

    def test_tag_names_the_image(self):
        with self.assertRaisesRegex(rc_mod.Refused, "does not name the image"):
            self.check(finished_img(0x9D, CONFIRMS, 300), "v2.1.14", tuya_h(300), {}, {})

    def test_resume_the_tag_itself_is_not_previous(self):
        tags = {"v2.1.12": 0x9C, "v2.1.13": 0x9D}
        prev = {"v2.1.12": tuya_h(), "v2.1.13": tuya_h(300)}
        lines = self.check(finished_img(0x9D, CONFIRMS, 300), "v2.1.13", tuya_h(300), tags, prev)
        self.assertIn("previous release v2.1.12", lines[0])

    def test_ring_mover_gate(self):
        big = finished_img(0xA0, CONFIRMS | RING_HIGH, 500, size=13 * KB)
        tags = {"v2.1.12": 0x9C, "v2.1.13": 0x9D, "v2.1.14": 0x9E}
        prev = {"v2.1.12": tuya_h(), "v2.1.13": tuya_h(300), "v2.1.14": tuya_h(400)}
        imgs = {"v2.1.13": finished_img(0x9D, CONFIRMS, 300), "v2.1.14": finished_img(0x9E, CONFIRMS, 400)}
        with self.assertRaisesRegex(rc_mod.Refused, "ring mover"):
            self.check(big, "v2.2.0", tuya_h(500), tags, prev, imgs)
        imgs["v2.1.14"] = finished_img(0x9E, CONFIRMS | RING_HIGH, 400, size=12 * KB)
        lines = self.check(big, "v2.2.0", tuya_h(500), tags, prev, imgs)
        self.assertIn("the ring mover v2.1.14", lines[-1])
        imgs["v2.1.14"] = finished_img(0x9E, CONFIRMS | RING_HIGH, 400, size=13 * KB)   # big itself: no mover
        with self.assertRaisesRegex(rc_mod.Refused, "ring mover"):
            self.check(big, "v2.2.0", tuya_h(500), tags, prev, imgs)
        # up to 12 KB needs no mover
        self.check(finished_img(0xA0, CONFIRMS, 500, size=12 * KB), "v2.2.0", tuya_h(500), tags, prev, {})

    def test_cli_with_a_git_repo(self):
        with tempfile.TemporaryDirectory() as d:
            def git(*a):
                subprocess.run(["git", "-C", d, "-c", "user.name=t", "-c", "user.email=t@example.invalid", *a],
                               check=True, capture_output=True)
            os.makedirs(os.path.join(d, "src"))
            hp = os.path.join(d, "src", "tuya.h")
            git("init", "-q")
            with open(hp, "w") as f:
                f.write(tuya_h())
            git("add", ".")
            git("commit", "-qm", "2.1.12")
            git("tag", "-a", "v2.1.12", "-m", "x")
            with open(hp, "w") as f:
                f.write(tuya_h(300))
            git("commit", "-qam", "2.1.13")
            git("tag", "-a", "v2.1.13", "-m", "x")
            with open(hp, "w") as f:
                f.write(tuya_h(400))
            git("commit", "-qam", "next")
            img = os.path.join(d, "app.img")
            with open(img, "wb") as f:
                f.write(finished_img(0x9E, CONFIRMS, 400))
            tool = os.path.join(TOOLS, "release", "release_check.py")
            p = subprocess.run([sys.executable, tool, "--image", img, "--tag", "v2.1.14", "--repo", d,
                                "--earlier-images", d], capture_output=True, text=True)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertIn("previous release v2.1.13: build 300", p.stdout)
            with open(img, "wb") as f:
                f.write(finished_img(0x9E, CONFIRMS, 500))
            with open(hp, "w") as f:
                f.write(tuya_h(500))
            p = subprocess.run([sys.executable, tool, "--image", img, "--tag", "v2.1.14", "--repo", d,
                                "--earlier-images", d], capture_output=True, text=True)
            self.assertNotEqual(p.returncode, 0)
            self.assertIn("must be 400", p.stderr)

    def test_release_sh_runs_the_rules_before_it_pushes(self):
        with open(os.path.join(TOOLS, "release", "release.sh")) as f:
            text = f.read()
        lines = text.splitlines()
        first = lambda pat: next(i for i, l in enumerate(lines) if re.search(pat, l) and not l.lstrip().startswith("#"))
        push = first(r"git push")
        self.assertLess(first(r"tools/release/release_check\.py"), push)
        self.assertLess(first(r"tools/fwimage\.py notes build-boot/boot\.bin"), push)
        self.assertLess(first(r"tools/changelog\.py"), push)          # the CHANGELOG.md requirement stays
        self.assertLess(first(r"unset FW_TEST VERSION_BYTE"), first(r"make test"))
        self.assertIn("tools/fwimage.py", re.search(r"FW_PATHS=\((.*)\)", text).group(1))


# --- CI ELF checks ------------------------------------------------------------------------------
ce = load("check_elf", os.path.join(TOOLS, "check_elf.py"))


class ElfChecks(unittest.TestCase):
    BOOT_NM = ("08000010 00000010 r k_boot_id\n08001000 A _flash_write_start\n08004800 A _flash_write_end\n"
               "20000418 B _ebss\n")

    def test_boot_ok(self):
        e, w, _ = ce.check_boot(ce.symbols(self.BOOT_NM), 3252)
        self.assertEqual((e, w), ([], []))

    def test_boot_errors(self):
        nm = self.BOOT_NM.replace("08000010 00000010", "08000014 00000010")
        self.assertIn("must be at 0x08000010", ce.check_boot(ce.symbols(nm), 3252)[0][0])
        nm = self.BOOT_NM.replace("08004800 A _flash_write_end", "08004000 A _flash_write_end")
        self.assertIn("_flash_write_end", ce.check_boot(ce.symbols(nm), 3252)[0][0])
        nm = self.BOOT_NM.replace("20000418 B _ebss", "20000A00 B _ebss")
        self.assertIn("reach past the mailbox", ce.check_boot(ce.symbols(nm), 3252)[0][0])
        self.assertIn("no k_boot_id", ce.check_boot(ce.symbols("20000418 B _ebss\n"), 2136)[0][0])
        e, w, _ = ce.check_boot(ce.symbols(self.BOOT_NM), 4097)
        self.assertTrue(any("more than" in x for x in e))
        e, w, _ = ce.check_boot(ce.symbols(self.BOOT_NM), 3585)
        self.assertEqual(e, [])
        self.assertIn("warning above 3584", w[0])
        self.assertEqual(ce.check_boot(ce.symbols(self.BOOT_NM), 3584)[1], [])

    def test_app_ring_high_matches_settings_start(self):
        low = "08004000 A _settings_start\n08005000 A _settings_end\n"
        high = "08004800 A _settings_start\n08005000 A _settings_end\n"
        self.assertEqual(ce.check_app(ce.symbols(low), finished_img())[0], [])
        self.assertEqual(ce.check_app(ce.symbols(high), finished_img(0x9D, CONFIRMS | RING_HIGH))[0], [])
        self.assertTrue(ce.check_app(ce.symbols(high), finished_img())[0])
        self.assertTrue(ce.check_app(ce.symbols(low), finished_img(0x9D, CONFIRMS | RING_HIGH))[0])
        guard = low + "08004000 A _flash_write_start\n08005000 A _flash_write_end\n"
        self.assertEqual(ce.check_app(ce.symbols(guard), finished_img())[0], [])
        bad = low + "08001000 A _flash_write_start\n08005000 A _flash_write_end\n"
        self.assertIn("write guard", ce.check_app(ce.symbols(bad), finished_img())[0][0])

    def test_ci_runs_the_checks(self):
        with open(os.path.join(REPO, ".github", "workflows", "ci.yml")) as f:
            ci = f.read()
        self.assertIn("python3 tools/check_elf.py boot build-boot/boot.elf build-boot/boot.bin", ci)
        self.assertIn('python3 tools/check_elf.py app "$d/zdms16_py32.elf" "$d/zdms16_py32.img"', ci)
        self.assertIn("run: make test", ci)
        self.assertIn("fwimage.py notes boot.bin zdms16_py32_full.bin", ci)
        self.assertIn("tools/fwimage.py", ci.split("sparse-checkout: |", 1)[1].split("sparse-checkout-cone-mode", 1)[0])


# --- fake chip tools for install.sh and make flash ---------------------------------------------
FAKE_PYOCD = r'''#!/usr/bin/env python3
# Fake pyOCD: the chip's option bytes, flash and mailbox boot word in $FAKE_STATE/state.json.
import json, os, sys
SF = os.path.join(os.environ["FAKE_STATE"], "state.json")
st = json.load(open(SF))
def save():
    json.dump(st, open(SF, "w"))
def log(*e):
    st["log"].append(list(e))
def reset(kind):
    log("reset", kind, st["ram"][0], st["ram"][1], st["halted_without_freeze"])
    st["halted"] = False
    if st.get("boot_v2"):            # the bootloader writes a fresh valid word (tries 1) at every boot
        w = 0xB2000001
        st["ram"] = [w, w ^ 0xFFFFFFFF]
def apply_ob():
    ob = bytes.fromhex(st.pop("pending_ob"))
    st["optr"] = (st["optr"] & 0xFFFF0000) | ob[0] | ob[1] << 8
    st["wrpr"] = ob[12] | ob[13] << 8
a = sys.argv[1:]
cmd = a[0]
if cmd == "commander":
    cs = [a[i + 1] for i, x in enumerate(a) if x == "-c"]
    for c in cs:
        p = c.split()
        if p[0] == "write32":
            addr, val = int(p[1], 0), int(p[2], 0)
            if addr == 0x20000BF8: st["ram"][0] = val
            elif addr == 0x20000BFC: st["ram"][1] = val
            elif addr == 0x40015808: st["iwdg_frozen"] = bool(val & 0x1000)
            elif addr == 0x40022014 and val & 0x08000000:
                if "pending_ob" in st: apply_ob()
                reset("obl")
        elif p[0] == "halt":
            if os.environ.get("FAKE_HALT_FAILS"):
                log("halt_failed"); save(); sys.exit("halt failed")
            if not st.get("iwdg_frozen"): st["halted_without_freeze"] = True
            st["halted"] = True
        elif p[0] == "read32":
            print("%08x:  %08x %08x                      |........|" % (int(p[1], 0), st["ram"][0], st["ram"][1]))
        elif p[0] == "reset":
            reset("commander")
        elif p[0] == "savemem":
            n = int(p[2], 0)
            open(p[3], "wb").write(bytes.fromhex(st["flash"])[:n])
    log("commander", *cs)
elif cmd == "load":
    base = int(a[a.index("--base-address") + 1], 0)
    f = [x for i, x in enumerate(a) if i > 0 and os.path.isfile(x) and a[i - 1] != "--pack"][0]
    data = open(f, "rb").read()
    if base == 0x1FFF0E80:          # active only after an option reload (OBL_LAUNCH) or a power-up
        st["pending_ob"] = data.hex()
        log("ob_write", data.hex())
        if "--no-reset" not in a: reset("load")
    else:
        if not st["wrpr"] & 1:
            log("load_refused_wrp"); save(); sys.exit("write protected")
        st["flash"] = data.hex()
        log("load", len(data))
        if "--no-reset" not in a: reset("load")
elif cmd == "reset":
    reset("reset")
save()
'''

FAKE_OPENOCD = r'''#!/usr/bin/env python3
import json, os
SF = os.path.join(os.environ["FAKE_STATE"], "state.json")
st = json.load(open(SF))
st["log"].append(["attach"])
json.dump(st, open(SF, "w"))
print("OPTR=0x%08x" % st["optr"])
print("WRPR=0x%08x" % st["wrpr"])
'''

FAKE_CURL = r'''#!/usr/bin/env python3
import os, shutil, sys
a = sys.argv[1:]
url = a[-1]
rel = os.environ["FAKE_RELEASE"]
if "-w" in a:
    print(url.replace("/releases/latest", "/releases/tag/" + os.environ["FAKE_TAG"]), end="")
    sys.exit(0)
out = a[a.index("-o") + 1]
name = url.rsplit("/", 1)[1]
tag = url.rsplit("/", 2)[1]
src = os.path.join(rel, name)
if tag != os.environ["FAKE_TAG"] or not os.path.exists(src):
    sys.exit(22)
shutil.copy(src, out)
'''


class FakeChip(unittest.TestCase):
    """install.sh and make flash against the fake tools above."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        d = self.tmp.name
        self.bin = os.path.join(d, "bin")
        os.makedirs(self.bin)
        for name, text in (("pyocd", FAKE_PYOCD), ("openocd", FAKE_OPENOCD), ("curl", FAKE_CURL)):
            p = os.path.join(self.bin, name)
            with open(p, "w") as f:
                f.write(text)
            os.chmod(p, 0o755)
        self.pack = os.path.join(d, "pack")
        open(self.pack, "w").close()
        self.rel = os.path.join(d, "release")
        os.makedirs(self.rel)

    def tearDown(self):
        self.tmp.cleanup()

    def chip(self, optr=0x0000F9AA, wrpr=0xFFFF, ram=(0xB2000003, 0xB2000003 ^ 0xFFFFFFFF), boot_v2=True):
        self.state = {"optr": optr, "wrpr": wrpr, "ram": list(ram), "flash": "", "log": [],
                      "boot_v2": boot_v2, "halted_without_freeze": False}
        with open(os.path.join(self.tmp.name, "state.json"), "w") as f:
            json.dump(self.state, f)

    def st(self):
        with open(os.path.join(self.tmp.name, "state.json")) as f:
            return json.load(f)

    def env(self, **kw):
        e = dict(os.environ, PATH=self.bin + os.pathsep + os.environ["PATH"], FAKE_STATE=self.tmp.name,
                 PYOCD=os.path.join(self.bin, "pyocd"), PACK=self.pack, FAKE_RELEASE=self.rel,
                 FAKE_TAG="v2.1.13", REPO_URL="https://example.invalid/o/r")
        for k in ("IMAGE", "OLD_BOOT", "ERASE_SETTINGS", "WRP", "YES"):
            e.pop(k, None)
        e.update({k: str(v) for k, v in kw.items()})
        return e

    def install(self, image=None, **kw):
        if isinstance(image, bytes):
            p = os.path.join(self.tmp.name, "full.bin")
            with open(p, "wb") as f:
                f.write(image)
            kw["IMAGE"] = p
        elif image is not None:
            kw["IMAGE"] = image
        p = subprocess.run(["bash", os.path.join(TOOLS, "swd", "install.sh")], env=self.env(**kw),
                           capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=120)
        return p.returncode, p.stdout + p.stderr

    def events(self, kind):
        return [e for e in self.st()["log"] if e[0] == kind]

    def assert_every_reset_clean(self):
        resets = self.events("reset")
        self.assertTrue(resets)
        for r in resets:
            self.assertEqual(r[2:4], [0, 0], f"reset with the boot word set: {r}")
        self.assertFalse(self.st()["halted_without_freeze"], "the core was halted without DBG_IWDG_STOP")

    def test_reflash(self):
        boot, img = boot_v2(), finished_img()
        full = full_img(boot, img)
        self.chip()
        rc, out = self.install(full)
        self.assertEqual(rc, 0, out)
        self.assertIn("bootloader: version 2 (features 0x0F", out)
        self.assertIn(f"boot sector CRC-32: 0x{fw.boot_sector_crc(boot):08X}", out)
        self.assertIn(f"DONE: firmware written and checked; boot sector CRC 0x{fw.boot_sector_crc(boot):08X}", out)
        self.assertEqual(bytes.fromhex(self.st()["flash"]), full)
        self.assertEqual(self.events("ob_write"), [])
        self.assertEqual(len(self.events("reset")), 2)          # pyOCD's after the load, then the final one
        self.assert_every_reset_clean()

    def test_stock_module(self):
        self.chip(optr=0x0000F855)                               # RDP on, no BOR
        rc, out = self.install(full_img(boot_v2(), finished_img()), YES=1)
        self.assertEqual(rc, 0, out)
        self.assertIn("read protection is ON", out)
        obs = self.events("ob_write")
        self.assertEqual(obs[0][1], "aaf95506ff0000ffffffffffffff0000")
        self.assertEqual(self.st()["optr"] & 0xFFFF, 0xF9AA)
        self.assertEqual([r[1] for r in self.events("reset")], ["load", "obl", "load", "commander"])
        self.assert_every_reset_clean()

    def test_stock_module_asks_first(self):
        self.chip(optr=0x0000F855)
        rc, out = self.install(full_img(boot_v2(), finished_img()))
        self.assertNotEqual(rc, 0)
        self.assertIn("removing it ERASES the stock firmware", out)
        self.assertEqual(self.events("ob_write"), [])
        self.assertEqual(self.events("load"), [])

    def test_final_reset_without_the_clear(self):
        """If the clear fails, the firmware is started all the same, with a warning."""
        self.chip()
        rc, out = self.install(full_img(boot_v2(), finished_img()), FAKE_HALT_FAILS=1)
        self.assertEqual(rc, 0, out)
        self.assertIn("warning: the boot word could not be cleared", out)
        self.assertEqual([r[1] for r in self.events("reset")], ["load", "reset"])

    def test_old_boot(self):
        full = full_img(boot_v1(), finished_img())
        self.chip()
        rc, out = self.install(full)
        self.assertNotEqual(rc, 0)
        self.assertIn("OLD_BOOT=1", out)
        self.assertEqual(self.st()["log"], [], "nothing done before the image is accepted")
        rc, out = self.install(full, OLD_BOOT=1)
        self.assertEqual(rc, 0, out)
        self.assertIn("v1 or older", out)
        rc, out = self.install(full, OLD_BOOT="yes")
        self.assertIn("OLD_BOOT must be 1 or unset", out)

    def test_erase_settings(self):
        full = full_img(boot_v2(), finished_img(0x9D, CONFIRMS | RING_HIGH, 300, size=13 * KB))
        self.chip()
        rc, out = self.install(full)
        self.assertNotEqual(rc, 0)
        self.assertIn("ERASE_SETTINGS=1", out)
        self.assertEqual(self.st()["log"], [])
        rc, out = self.install(full, ERASE_SETTINGS=1)
        self.assertEqual(rc, 0, out)
        self.assertIn("the settings in sector 4 are erased", out)

    def test_sim_refused(self):
        self.chip()
        rc, out = self.install(full_img(boot_v2(), finished_img(0xBF, SIM | CONFIRMS, 0xFFFE)))
        self.assertNotEqual(rc, 0)
        self.assertIn("SIM bench build", out)

    def test_wrp(self):
        self.chip()
        rc, out = self.install(full_img(boot_v2(), finished_img()), WRP=1)
        self.assertEqual(rc, 0, out)
        log = self.st()["log"]
        kinds = [e[0] if e[0] != "reset" else "reset:" + e[1] for e in log if e[0] in ("load", "ob_write", "reset")]
        self.assertEqual(kinds, ["load", "reset:load", "ob_write", "reset:load", "reset:obl", "reset:commander"])
        self.assertEqual(self.events("ob_write")[0][1], "aaf95506ff0000fffffffffffeff0100")   # WRPR 0xFFFE
        self.assertEqual(self.st()["wrpr"], 0xFFFE)
        self.assertIn("sector 0 write-protected", out)
        self.assert_every_reset_clean()
        # a re-flash clears the protection first, WRP=1 or not
        rc, out = self.install(full_img(boot_v2(), finished_img(0x9E, CONFIRMS, 400)))
        self.assertEqual(rc, 0, out)
        self.assertIn("write protection is on (WRPR=0x0000fffe): cleared first", out)
        self.assertEqual(self.st()["wrpr"], 0xFFFF)
        self.assertNotIn(["load_refused_wrp"], self.st()["log"])

    def release_files(self, sums_prefix="", tamper=None, drop=None):
        boot, img = boot_v2(), finished_img()
        files = {"zdms16_py32_full.bin": full_img(boot, img), "boot.bin": boot, "zdms16_py32.img": img,
                 "secondary_mcu_image.c": b"x"}
        sums = "".join(f"{hashlib.sha256(v).hexdigest()}  {sums_prefix}{k}\n" for k, v in files.items() if k != drop)
        if tamper:
            files[tamper] = files[tamper][:-1] + bytes([files[tamper][-1] ^ 1])
        for k, v in files.items():
            with open(os.path.join(self.rel, k), "wb") as f:
                f.write(v)
        with open(os.path.join(self.rel, "SHA256SUMS"), "w") as f:
            f.write(sums)
        return files

    def test_release_image(self):
        files = self.release_files()
        self.chip()
        rc, out = self.install("release")
        self.assertEqual(rc, 0, out)
        self.assertIn("release v2.1.13", out)
        self.assertIn("SHA256SUMS of v2.1.13: full image, bootloader and app image OK", out)
        self.assertEqual(bytes.fromhex(self.st()["flash"]), files["zdms16_py32_full.bin"])
        self.chip()
        rc, out = self.install("release:v2.1.13")
        self.assertEqual(rc, 0, out)
        rc, out = self.install("release:v2.1.99")
        self.assertNotEqual(rc, 0)
        self.assertIn("could not download", out)

    def test_release_old_style_sums(self):
        self.release_files(sums_prefix="build/")
        self.chip()
        rc, out = self.install("release")
        self.assertEqual(rc, 0, out)

    def test_release_checks(self):
        for kw, msg in (({"tamper": "boot.bin"}, "do not match its SHA256SUMS"),
                        ({"tamper": "zdms16_py32_full.bin"}, "do not match its SHA256SUMS"),
                        ({"drop": "boot.bin"}, "does not list boot.bin")):
            self.release_files(**kw)
            self.chip()
            rc, out = self.install("release")
            self.assertNotEqual(rc, 0, kw)
            self.assertIn(msg, out)
            self.assertEqual(self.events("load"), [])
        # a full image that is not its boot.bin + image, with sums that match
        files = self.release_files()
        bad = bytearray(files["zdms16_py32_full.bin"])
        bad[0x0FFF] = 0
        with open(os.path.join(self.rel, "zdms16_py32_full.bin"), "wb") as f:
            f.write(bad)
        sums = ""
        for k in files:
            with open(os.path.join(self.rel, k), "rb") as f:
                sums += f"{hashlib.sha256(f.read()).hexdigest()}  {k}\n"
        with open(os.path.join(self.rel, "SHA256SUMS"), "w") as f:
            f.write(sums)
        self.chip()
        rc, out = self.install("release")
        self.assertNotEqual(rc, 0)
        self.assertIn("is not its boot.bin + app image", out)

    def test_make_flash(self):
        full = full_img(boot_v2(), finished_img())
        build = os.path.join(self.tmp.name, "b")
        os.makedirs(build)
        with open(os.path.join(build, "zdms16_py32_full.bin"), "wb") as f:
            f.write(full)
        self.chip()
        p = subprocess.run(["make", "-s", "-C", REPO, "flash", f"BUILD={build}", "-o", f"{build}/zdms16_py32_full.bin",
                            f"PYOCD={os.path.join(self.bin, 'pyocd')}", f"PACK={self.pack}"],
                           env=self.env(), capture_output=True, text=True)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertEqual(bytes.fromhex(self.st()["flash"]), full)
        self.assertEqual([r[1] for r in self.events("reset")], ["load", "commander"])
        self.assert_every_reset_clean()
        # an image over 12 KB would erase the settings sector
        with open(os.path.join(build, "zdms16_py32_full.bin"), "wb") as f:
            f.write(full_img(boot_v2(), finished_img(0x9D, CONFIRMS | RING_HIGH, 300, size=13 * KB)))
        self.chip()
        p = subprocess.run(["make", "-s", "-C", REPO, "flash", f"BUILD={build}", "-o", f"{build}/zdms16_py32_full.bin",
                            f"PYOCD={os.path.join(self.bin, 'pyocd')}", f"PACK={self.pack}"],
                           env=self.env(), capture_output=True, text=True)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("ERASE_SETTINGS=1", p.stderr)
        self.assertEqual(self.st()["log"], [])


# --- Makefile -------------------------------------------------------------------------------
class Makefile(unittest.TestCase):
    def make(self, *args, cwd=REPO):
        p = subprocess.run(["make", "-n", "--no-print-directory", "-C", cwd, *args], capture_output=True,
                           text=True, env=dict(os.environ, LC_ALL="C"))
        return p.returncode, p.stdout + p.stderr

    def test_fw_test_range(self):
        for bad in ("0", "100", "05", "x"):
            rc, out = self.make(f"FW_TEST={bad}")
            self.assertNotEqual(rc, 0, bad)
            self.assertIn("a test build takes 1..99", out)
        rc, out = self.make("FW_TEST=5", "SIM=1")
        self.assertNotEqual(rc, 0)
        self.assertIn("fixed build 0xFFFE", out)

    def test_fw_test_plumbing(self):
        # -B: as if nothing were built yet; -o: the linked binary as it is (no SDK needed)
        rc, out = self.make("-B", "FW_TEST=5", "build-test5/zdms16_py32.img", "-o", "build-test5/zdms16_py32.bin")
        self.assertEqual(rc, 0, out)
        self.assertIn("tools/mkimage.py build-test5/zdms16_py32.bin build-test5/zdms16_py32.img --test 5", out)
        rc, out = self.make("-B", "FW_TEST=5", "build-test5/main.o")
        self.assertEqual(rc, 0, out)
        self.assertIn("-DFW_TEST=5", out)
        rc, out = self.make("-B", "build/main.o")
        self.assertIn("src/main.c", out)
        self.assertNotIn("-DFW_TEST", out)
        rc, out = self.make("-B", "build/zdms16_py32.img", "-o", "build/zdms16_py32.bin")
        self.assertIn("tools/mkimage.py build/zdms16_py32.bin build/zdms16_py32.img", out)
        self.assertNotIn("--test", out)

    def test_test_boot_is_part_of_make_test(self):
        """make test-boot (the bootloader harness, docs/BOOTLOADER.md section 11) runs in make test,
        which CI runs: the gate before any SWD install."""
        rc, boot = self.make("test-boot")
        if rc != 0:
            self.skipTest("no test-boot target here yet: it comes with the bootloader harness (tests/boot/)")
        rc, out = self.make("test")
        self.assertEqual(rc, 0, out)
        for line in (x for x in boot.splitlines() if x.strip()):
            self.assertIn(line, out)

    def test_boot_size_report(self):
        with tempfile.TemporaryDirectory() as d:
            fake = os.path.join(d, "size")
            with open(fake, "w") as f:
                f.write("#!/bin/sh\n[ \"$1\" = -B ] && printf '   text\\t   data\\t    bss\\t    dec\\t    hex\\tfilename\\n"
                        "   %s\\t     48\\t    520\\t      0\\t      0\\tboot.elf\\n' \"$FAKE_TEXT\"\nexit 0\n")
            os.chmod(fake, 0o755)
            for text, warn, ok in (("3200", False, True), ("3537", True, True), ("4049", True, False)):
                p = subprocess.run(["make", "-s", "-f", os.path.join(REPO, "Makefile"), "build-boot/boot.elf",
                                    "CC=true", f"SIZE={fake}", "BOOT_OBJS="], cwd=d, capture_output=True, text=True,
                                   env=dict(os.environ, FAKE_TEXT=text))
                out = p.stdout + p.stderr
                n = int(text) + 48
                self.assertIn(f"bootloader: {n} of 4096 bytes", out)
                self.assertEqual("warning: the bootloader has only" in out, warn and ok, out)
                self.assertEqual(p.returncode == 0, ok, out)


# --- ota_swd_emu: against a model of the bootloader's SWD interface ---------------------------
emu = load("ota_swd_emu", os.path.join(TOOLS, "ota_swd_emu.py"))


class BootModel:
    """The RAM a bootloader shows over SWD (g_inject, g_boot, g_req) and its update loop: v2 asks
    for the header first, then blocks of up to 128 bytes that fit the page, and starts the app
    (VTOR) when the image is done."""
    INJ, INJL, INJP, BOOT, REQ, RX, HEAD = 0x20000100, 0x20000300, 0x20000302, 0x20000310, 0x20000340, 0x20000400, 0x20000480

    def __init__(self, img, v2=True, word=(0xB2000002, 0xB2000002 ^ 0xFFFFFFFF)):
        self.mem, self.img, self.v2 = {}, img, v2
        self.size = len(img)
        self.vtor = 0
        self.resets, self.answers, self.written = [], [], bytearray()
        self.pinned, self.expect, self.requests = False, 0, 0
        self.w32(0x20000BF8, word[0])
        self.w32(0x20000BFC, word[1])
        self.offer = None

    def syms(self):
        boot = {"g_inject": [(self.INJ, 150)], "g_inject_len": [(self.INJL, 2)],
                "g_inject_pos": [(self.INJP, 2)], "g_boot": [(self.BOOT, 32 if self.v2 else 28)]}
        if self.v2:
            boot["k_boot_id"] = [(0x08000010, 16)]
            boot["g_req"] = [(self.REQ, 16)]
        app = {"g_rx": [(self.RX, 0x80)], "g_head": [(self.HEAD, 1)]}
        return app, boot

    # memory
    def read8(self, a): return self.mem.get(a, 0)
    def write8(self, a, v):
        self.mem[a] = v & 0xFF
        if a == self.HEAD:
            self.app_got_frame()
    def r(self, a, n): return int.from_bytes(bytes(self.read8(a + i) for i in range(n)), "little")
    def w(self, a, v, n):
        for i in range(n):
            self.mem[a + i] = (v >> (8 * i)) & 0xFF
    def read16(self, a): return self.r(a, 2)
    def read32(self, a): return self.vtor if a == emu.VTOR else self.r(a, 4)
    def w32(self, a, v): self.w(a, v, 4)
    def write32(self, a, v): self.w32(a, v)
    def read_memory_block8(self, a, n): return [self.read8(a + i) for i in range(n)]
    def read_memory_block32(self, a, n): return [self.r(a + 4 * i, 4) for i in range(n)]
    def write_memory_block8(self, a, data):
        for i, b in enumerate(data):
            self.mem[a + i] = b
    def flush(self): pass
    def write16(self, a, v):
        self.w(a, v, 2)
        if a == self.INJL and v:
            self.boot_got_frame(bytes(self.read_memory_block8(self.INJ, v)))
            self.w(self.INJP, v, 2)
    def reset(self):
        self.resets.append((self.r(0x20000BF8, 4), self.r(0x20000BFC, 4)))
        self.vtor = 0

    # the app: an 0xA0 offer in its ring -> restart into the bootloader (asked)
    def app_got_frame(self):
        f = bytes(self.read8(self.RX + i) for i in range(self.read8(self.HEAD)))
        self.offer = f
        self.ask()

    def ask(self):
        if self.v2 and not self.pinned:
            off, n = 0xC0, 16
        else:
            off, n = self.expect, min(128 - self.expect % 128, self.size - self.expect)
            if not self.v2:
                self.w32(self.BOOT, off)
        self.requests += 1
        self.w32(self.BOOT + 16, self.requests)
        if self.v2:
            self.write_memory_block8(self.REQ, list(struct.pack(">IH", off, n) + bytes([2, 1]) + bytes(8)))

    def boot_got_frame(self, f):
        assert f[:3] == b"\x55\xaa\x02" and f[5] == 0xA1 and sum(f[:-1]) & 0xFF == f[-1]
        d = f[8:-1]
        off, size, crc = struct.unpack_from(">III", d, 0)
        data = d[13:]
        self.answers.append((off, len(data)))
        if self.v2 and not self.pinned:
            assert off == 0xC0 and data == self.img[0xC0:0xD0]
            self.pinned, self.expect = True, 0
            self.w32(self.BOOT + 4, size)
            self.w(self.BOOT + 0x1C, struct.unpack_from("<H", data, 14)[0], 2)
        else:
            assert off == self.expect and 0 < len(data) <= 128
            self.written += data
            self.expect += len(data)
        if self.expect == self.size:
            self.vtor = 0x08001000
            return
        self.ask()


class OtaSwdEmu(unittest.TestCase):
    def session(self, model, img):
        app, boot = model.syms()
        return emu.Session(model, img, app, boot, out=lambda *_: None)

    def test_offer_has_the_build(self):
        img = finished_img(0x9D, CONFIRMS, 301, size=0x300)
        f = emu.frame(0xA0, emu.offer(img))
        self.assertEqual(f[5:8], bytes([0xA0, 0, 11]))
        h = fw.header(img)
        self.assertEqual(f[8:19], bytes([0x9D]) + struct.pack(">IIH", h.size, h.crc, 301))

    def test_answers_are_stateless_blocks(self):
        img = finished_img(size=0x300)
        a = emu.answer(img, 0xC0, 16)
        self.assertEqual(a[:13], struct.pack(">III", 0xC0, len(img), fw.header(img).crc) + bytes([0x9D]))
        self.assertEqual(a[13:], img[0xC0:0xD0])
        self.assertEqual(len(emu.answer(img, 0, 128)) - 13, 64)          # the ZTU's BLOCK_MAX
        self.assertEqual(len(emu.answer(img, 0x2F0, 128)) - 13, 16)
        self.assertEqual(len(emu.answer(img, 0x300, 128)) - 13, 0)       # past the end
        self.assertEqual(emu.parse_request(struct.pack(">IH", 0xC0, 16) + bytes([2, 3]) + bytes(8)), (0xC0, 16, 2, 3))

    def test_v2_update_header_first(self):
        img = finished_img(0x9D, CONFIRMS, 301, size=0x340)
        m = BootModel(img)
        s = self.session(m, img)
        sent, stopped = s.run()
        self.assertEqual((s.bstate()["build"], s.bstate()["size"]), (301, len(img)))   # v2 layout read
        self.assertFalse(stopped)
        self.assertEqual(m.offer[5:8], bytes([0xA0, 0, 11]))
        self.assertEqual(m.answers[0], (0xC0, 16))                       # the header first
        self.assertEqual(bytes(m.written), img)
        self.assertEqual(m.vtor, 0x08001000)
        self.assertEqual(m.resets, [])

    def test_v1_bootloader(self):
        img = finished_img(0x9D, CONFIRMS, 301, size=0x340)
        m = BootModel(img, v2=False)
        self.session(m, img).run()
        self.assertEqual(m.answers[0], (0, 64))
        self.assertEqual(bytes(m.written), img)

    def test_power_cut_clears_the_boot_word_before_its_reset(self):
        img = finished_img(0x9D, CONFIRMS, 301, size=0x340)
        for v2 in (True, False):
            m = BootModel(img, v2=v2)
            sent, stopped = self.session(m, img).run(stop=0x100)
            self.assertTrue(stopped)
            self.assertEqual(m.resets, [(0, 0)])
            self.assertEqual(len(m.written), 0x100)

    def test_no_offer(self):
        img = finished_img(size=0x340)
        m = BootModel(img)
        m.ask()                                                           # a bootloader that already asks
        self.session(m, img).run(stop=-1)
        self.assertIsNone(m.offer)
        self.assertEqual(bytes(m.written), img)

    def test_v2_without_request_buffer(self):
        m = BootModel(finished_img())
        app, boot = m.syms()
        del boot["g_req"]
        with self.assertRaises(SystemExit):
            emu.Session(m, finished_img(), app, boot)


# --- gen_curves, changelog ----------------------------------------------------------------
def load_gen_curves():
    return load("gen_curves", os.path.join(TOOLS, "gen_curves.py"))


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


class Changelog(unittest.TestCase):
    def load(self):
        return load("changelog", os.path.join(TOOLS, "changelog.py"))

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
