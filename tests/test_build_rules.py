#!/usr/bin/env python3
"""The build number rules (docs/BOOTLOADER.md section 2) through the Makefile and src/tuya.h:
what each make command line builds, and what must not build at all. FW_BUILD is the release
build, a literal in src/tuya.h that nothing overrides; test builds move it by 1..99 only
(FW_TEST above it, FW_PREV=1 FW_TEST=n below it), so a typo cannot jump the build space; bench
test B11's switch (LINK_WDG_TEST) builds only below the release, so a unit on the release never
takes such an image. Each case asks the Makefile for its defines (make print-defs, no SDK or
ARM toolchain needed) and compiles src/tuya.h with them on the host.
Run: python3 tests/test_build_rules.py (part of make test)"""
import os, re, subprocess, sys, unittest

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
CC = os.environ.get("HOST_CC", "cc")


def release_build():
    with open(os.path.join(ROOT, "src", "tuya.h")) as f:
        return int(re.search(r"^#define FW_BUILD\s+(\d+)\s*$", f.read(), re.M).group(1))


RELEASE = release_build()


def make_defs(*args):
    """(returncode, build dir, defines, make's messages) for make print-defs with args."""
    env = {k: v for k, v in os.environ.items() if k not in ("MAKEFLAGS", "MAKELEVEL", "MFLAGS")}
    p = subprocess.run(["make", "-s", "--no-print-directory", "print-defs", *args], cwd=ROOT,
                       env=env, capture_output=True, text=True)
    if p.returncode:
        return p.returncode, None, None, p.stderr
    words = p.stdout.split()
    return 0, words[0], [w for w in words[1:] if w.startswith("-D")], p.stderr


def compile_tuya_h(defs, build=None):
    """Compile src/tuya.h with defs (and, if given, check FW_IMAGE_BUILD): (ok, compiler output)."""
    src = '#include "tuya.h"\n'
    if build is not None:
        src += "_Static_assert(FW_IMAGE_BUILD == %du, \"build\");\n" % build
    p = subprocess.run([CC, "-std=c11", "-fsyntax-only", "-Werror", "-Isrc", *defs, "-x", "c", "-"],
                       cwd=ROOT, input=src, capture_output=True, text=True)
    return p.returncode == 0, p.stderr


class BuildRules(unittest.TestCase):
    def builds(self, args, build, build_dir):
        rc, bdir, defs, err = make_defs(*args)
        self.assertEqual(rc, 0, "make %s: %s" % (" ".join(args), err))
        self.assertEqual(bdir, build_dir, " ".join(args))
        ok, out = compile_tuya_h(defs, build)
        self.assertTrue(ok, "make %s should build %d: %s" % (" ".join(args), build, out))

    def refused_by_make(self, args, why):
        rc, _, _, err = make_defs(*args)
        self.assertNotEqual(rc, 0, "make %s was accepted" % " ".join(args))
        self.assertIn(why, err)

    def refused(self, args, why):
        rc, _, defs, err = make_defs(*args)
        self.assertEqual(rc, 0, "make %s: %s" % (" ".join(args), err))
        ok, out = compile_tuya_h(defs)
        self.assertFalse(ok, "make %s compiled" % " ".join(args))
        self.assertIn(why, out)

    def test_release_literal(self):
        self.assertEqual(RELEASE % 100, 0)
        self.assertGreaterEqual(RELEASE, 300)                       # release R is the first

    def test_normal_and_bench_builds(self):
        self.builds([], RELEASE, "build")
        self.builds(["DIAG=1"], RELEASE, "build-diag")
        self.builds(["MEASURE=1"], RELEASE, "build-measure")
        self.builds(["SIM=1"], 0xFFFE, "build-sim")

    def test_spare_test_builds_stay_below_the_next_release(self):
        self.builds(["FW_TEST=1"], RELEASE + 1, "build-t1")
        self.builds(["FW_TEST=7"], RELEASE + 7, "build-t7")
        self.builds(["FW_TEST=99"], RELEASE + 99, "build-t99")
        self.refused(["FW_TEST=0"], "1..99")
        self.refused(["FW_TEST=100"], "1..99")
        self.refused(["FW_TEST=6400"], "1..99")

    def test_builds_below_the_release(self):
        """B3 and B11: a test build of the previous release, below this one."""
        self.builds(["FW_PREV=1", "FW_TEST=99"], RELEASE - 1, "build-prev-t99")
        if RELEASE - 100 + 1 >= 256:
            self.builds(["FW_PREV=1", "FW_TEST=1"], RELEASE - 99, "build-prev-t1")
        else:                                                      # release R: 201..255 are no builds
            self.builds(["FW_PREV=1", "FW_TEST=56"], 256, "build-prev-t56")
            self.refused(["FW_PREV=1", "FW_TEST=55"], "256..0xFFFD")
        self.refused(["FW_PREV=1"], "FW_PREV=1 FW_TEST=n")          # would be the previous release
        self.refused(["FW_PREV=0", "FW_TEST=5"], "FW_PREV=1 FW_TEST=n")
        self.refused(["FW_PREV=2", "FW_TEST=5"], "FW_PREV=1 FW_TEST=n")

    def test_the_release_build_cannot_be_overridden(self):
        self.refused_by_make(["FW_BUILD=1000"], "FW_BUILD is the release build")
        self.refused_by_make(["FW_BUILD=200", "FW_TEST=99"], "FW_BUILD is the release build")
        ok, out = compile_tuya_h(["-DFW_BUILD=6500"])                # not through make either
        self.assertFalse(ok)
        self.assertIn("FW_BUILD is the release build", out)

    def test_link_watchdog_bench_switch_only_below_the_release(self):
        self.builds(["LINK_WDG_TEST=1", "FW_PREV=1", "FW_TEST=99"], RELEASE - 1,
                    "build-prev-t99-linktest")
        self.refused(["LINK_WDG_TEST=1"], "LINK_WDG_TEST builds lie below the release")
        self.refused(["LINK_WDG_TEST=1", "FW_TEST=1"], "LINK_WDG_TEST builds lie below the release")
        self.refused(["LINK_WDG_TEST=1", "FW_TEST=99"], "LINK_WDG_TEST builds lie below the release")
        for bench in ("SIM=1", "MEASURE=1", "DIAG=1"):
            self.refused(["LINK_WDG_TEST=1", "FW_PREV=1", "FW_TEST=99", bench],
                         "a normal build's bench switch")
        self.refused_by_make(["LINK_WDG_TEST=yes"], "LINK_WDG_TEST=1 (bench test B11) or 0")
        self.refused_by_make(["LINK_WDG_TEST=2"], "LINK_WDG_TEST=1 (bench test B11) or 0")
        self.builds(["LINK_WDG_TEST=0"], RELEASE, "build")                # off


if __name__ == "__main__":
    sys.exit(0 if unittest.main(verbosity=1, exit=False).result.wasSuccessful() else 1)
