#!/usr/bin/env python3
"""Release rules of docs/BOOTLOADER.md sections 1, 2 and 11, checked by tools/release/release.sh
before it tags anything:
  - the image's build is FW_BUILD of src/tuya.h (no FW_TEST, no override);
  - FW_BUILD is the previous release tag's + 100 (git show <tag>:src/tuya.h), or 300 when the
    previous release has none (2.1.x), so every release is above every test build before it;
  - the version byte is above every other release tag's (tags are vX.Y.Z of the byte: unique);
  - the first release over 12 KB needs an earlier release with RING_HIGH of at most 12 KB (the
    ring mover, section 1); the earlier releases' images are read from GitHub (gh), or from
    --earlier-images DIR (<tag>.img files);
  - the room for later fixes, printed together (section 12): the app's free bytes in its layout
    (12 KB, 14 KB with RING_HIGH), refused below HEADROOM_MIN (CI's warning line) unless
    --accept-headroom names exactly the free bytes (the user's decision for this release), and
    the ZTU OTA margin: --ztu-ota FILE, an OTA file of the fork's ZDMS16-2 build, is measured
    with this app carried instead of the image it carries, refused over the 0x34000 limit (or
    within ZTU_OTA_SLACK of it), with a warning when a 14 KB app (after the ring move) would not
    fit; --ztu-ota skip leaves it out, said so.

    release_check.py --image build/zdms16_py32.img --tag vX.Y.Z --ztu-ota FILE|skip [--repo DIR]
                     [--gh-repo OWNER/NAME] [--earlier-images DIR] [--accept-headroom N]
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True                         # no __pycache__ in tools/
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import fwimage as fw                                   # noqa: E402

TAG_RE = re.compile(r"v(\d+)\.(\d+)\.(\d+)$")
DEFINE_RE = re.compile(r"^\s*#\s*define\s+FW_BUILD\s+(0[xX][0-9A-Fa-f]+|\d+)[uU]?\b", re.M)


HEADROOM_MIN = 512              # free app bytes; CI warns below as many (ci.yml HEADROOM_WARN_BYTES)
ZTU_OTA_MAX = 0x34000           # the fork's FLASH_OTA_IMAGE_MAX_SIZE: the largest OTA file it takes
ZTU_OTA_SLACK = 16              # the carried array's alignment and the OTA file's padding


class Refused(Exception):
    pass


def tag_byte(tag):
    """The version byte a release tag names, or None for other tags."""
    m = TAG_RE.match(tag)
    if not m:
        return None
    major, minor, patch = (int(x) for x in m.groups())
    if major > 3 or minor > 3 or patch > 15:
        return None
    return (major << 6) | (minor << 4) | patch


def fw_build_of(text):
    """The release build that a src/tuya.h defines (FW_BUILD), 0 when it has none (2.1.x). The
    SIM build's fixed 0xFFFE is not a release build."""
    if text is None:
        return 0
    values = {int(v, 0) for v in DEFINE_RE.findall(text)} - {fw.BUILD_SIM}
    if len(values) > 1:
        raise Refused(f"src/tuya.h defines FW_BUILD as {sorted(values)}: one release build expected")
    return values.pop() if values else 0


def git(repo, *args):
    p = subprocess.run(["git", "-C", repo, *args], capture_output=True, text=True)
    return p.stdout if p.returncode == 0 else None


def release_tags(repo):
    """{tag: version byte} of the vX.Y.Z tags in the repo."""
    out = git(repo, "tag", "--list", "v*") or ""
    tags = {}
    for t in out.split():
        b = tag_byte(t)
        if b is not None:
            tags[t] = b
    return tags


def gh_image(gh_repo, tag, tmp):
    d = os.path.join(tmp, tag)
    os.makedirs(d, exist_ok=True)
    p = subprocess.run(["gh", "release", "download", tag, "--repo", gh_repo, "--pattern",
                        "zdms16_py32.img", "--dir", d], capture_output=True, text=True)
    path = os.path.join(d, "zdms16_py32.img")
    if p.returncode != 0 or not os.path.exists(path):
        return None
    with open(path, "rb") as f:
        return f.read()


def check(image, tag, head_tuya_h, tags, tuya_h_of, image_of):
    """Returns the lines to print; raises Refused. tags: {tag: byte}; tuya_h_of(tag) -> text or
    None; image_of(tag) -> released image bytes or None."""
    h = fw.finished(image)
    vb = tag_byte(tag)
    if vb is None or vb != h.version:
        raise Refused(f"tag {tag} does not name the image's version byte 0x{h.version:02X}")
    head_build = fw_build_of(head_tuya_h)
    if head_build == 0:
        raise Refused("src/tuya.h has no FW_BUILD: every release from build 300 on has one")
    if h.build != head_build:
        raise Refused(f"the normal build has build {h.build}, src/tuya.h says FW_BUILD {head_build} "
                      "(FW_TEST or an override in the environment?)")
    others = {t: b for t, b in tags.items() if t != tag}
    higher = sorted(t for t, b in others.items() if b >= vb)
    if higher:
        raise Refused(f"version byte 0x{vb:02X} ({tag}) is not above the release tag(s) {', '.join(higher)}: "
                      "bump FW_VERSION in src/tuya.h")
    lines = []
    prev = max(others, key=others.get) if others else None
    prev_build = fw_build_of(tuya_h_of(prev)) if prev else 0
    want = prev_build + fw.BUILD_STEP if prev_build else fw.BUILD_FIRST
    if h.build != want:
        raise Refused(f"FW_BUILD {h.build}: the release after {prev or 'none'} (build {prev_build or 'none'}) "
                      f"must be {want} (previous release + {fw.BUILD_STEP}; the first one {fw.BUILD_FIRST})")
    if h.build > fw.BUILD_MAX:
        raise Refused(f"build {h.build} is beyond 0x{fw.BUILD_MAX:X}")
    lines.append(f"release {tag}: build {h.build} (previous release {prev or 'none'}: "
                 f"build {prev_build or 'none'}), {h.size} bytes, flags 0x{h.flags:02X}")
    if h.size > fw.APP_LIMIT_LEGACY:
        if not h.flags & fw.FLAG_RING_HIGH:
            raise Refused(f"app of {h.size} bytes without RING_HIGH")
        mover = None
        for t in sorted(others, key=others.get):
            img = image_of(t)
            if img is None or len(img) < fw.APP_SIZE_MIN:
                continue
            eh = fw.header(img)
            if eh.flags & fw.FLAG_RING_HIGH and eh.size <= fw.APP_LIMIT_LEGACY:
                mover = t
                break
        if mover is None:
            raise Refused(f"the first release over 12 KB ({h.size} bytes) needs an earlier release with "
                          "RING_HIGH of at most 12 KB (the ring mover, docs/BOOTLOADER.md section 1), "
                          "spread to every module and reporting 'ring moved'")
        lines.append(f"over 12 KB: the ring mover {mover} was released before")
    return lines


def carried(ota):
    """(offset, header) of the app image a ZTU OTA file carries, found by its header (the magic
    at image offset 0xC0, then size and CRC); None when it carries none."""
    magic = struct.pack("<I", fw.APP_MAGIC)
    found = []
    pos = ota.find(magic)
    while pos >= 0:
        start = pos - fw.APP_HEADER_OFFSET
        if start >= 0 and start + fw.APP_SIZE_MIN <= len(ota):
            size = fw.header(ota[start:start + fw.APP_SIZE_MIN]).size
            if fw.APP_SIZE_MIN <= size <= fw.BOOT_APP_MAX and start + size <= len(ota):
                try:
                    found.append((start, fw.finished(ota[start:start + size])))
                except fw.Refused:
                    pass
        pos = ota.find(magic, pos + 1)
    if len(found) > 1:
        raise Refused(f"the ZTU OTA file holds {len(found)} app images: one expected")
    return found[0] if found else None


def budget(image, ota=None, ota_name="", accept=None):
    """The room left for later fixes (docs/BOOTLOADER.md section 12), as (lines, warnings);
    raises Refused. ota: the bytes of a ZTU OTA file of the ZDMS16-2 build, or None (not
    checked); accept: the free bytes the user accepted below HEADROOM_MIN, or None."""
    h = fw.finished(image)
    room = fw.BOOT_APP_MAX if h.flags & fw.FLAG_RING_HIGH else fw.APP_LIMIT_LEGACY
    free = room - h.size
    lines = [f"app headroom: {h.size} of {room} bytes, {free} free (at least {HEADROOM_MIN})"]
    warnings = []
    if free < HEADROOM_MIN:
        if accept != free:
            raise Refused(f"the app leaves {free} bytes free of {room}, below {HEADROOM_MIN}: the next fix may "
                          "not fit. Recover room first, or record the user's decision for this release: "
                          f"ACCEPT_HEADROOM={free} tools/release/release.sh")
        warnings.append(f"app headroom {free} bytes, below {HEADROOM_MIN}: accepted for this release "
                        f"(ACCEPT_HEADROOM={free})")
    elif accept is not None:
        warnings.append(f"ACCEPT_HEADROOM={accept} is not needed: {free} bytes are free")
    if ota is None:
        lines.append("ZTU OTA margin: not checked (ZTU_OTA=skip)")
        warnings.append("the ZTU OTA margin was not checked: the py32-bump build checks the 0x34000 limit only "
                        "after the tag")
        return lines, warnings
    c = carried(ota)
    if c is None:
        raise Refused(f"{ota_name}: no app image in it (not an OTA file of the ZDMS16-2 build?)")
    ch = c[1]
    with_this = len(ota) - ch.size + h.size
    margin = ZTU_OTA_MAX - with_this
    lines.append(f"ZTU OTA margin: {ota_name} is {len(ota)} bytes carrying {fw.version_str(ch.version)} "
                 f"({ch.size} bytes); with this app ~{with_this} bytes, {margin} below the {ZTU_OTA_MAX} "
                 f"(0x{ZTU_OTA_MAX:X}) limit")
    if margin < ZTU_OTA_SLACK:
        where = "over" if margin < 0 else f"within {ZTU_OTA_SLACK} bytes of"
        raise Refused(f"the ZTU OTA file would be ~{with_this} bytes, {where} the 0x{ZTU_OTA_MAX:X} limit: the "
                      "Zigbee firmware could not carry this app (free room in the ZTU build first)")
    grow = fw.BOOT_APP_MAX - h.size
    if margin < grow:
        warnings.append(f"ZTU OTA margin {margin} bytes: a 14 KB app (after the ring move, section 1) needs "
                        f"{grow} more, {grow - margin} short; the ZTU build must free room before such an app")
    return lines, warnings


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--image", required=True)
    ap.add_argument("--tag", required=True)
    ap.add_argument("--repo", default=".")
    ap.add_argument("--gh-repo", default="patapovich/zdms16-py32")
    ap.add_argument("--earlier-images")
    ap.add_argument("--ztu-ota", required=True, help="OTA file of the fork's ZDMS16-2 build, or 'skip'")
    ap.add_argument("--accept-headroom", type=int, help="the free bytes accepted below the minimum")
    a = ap.parse_args(argv[1:])
    ota = None
    if a.ztu_ota != "skip":
        with open(a.ztu_ota, "rb") as f:
            ota = f.read()
    with open(a.image, "rb") as f:
        image = f.read()
    with open(os.path.join(a.repo, "src", "tuya.h")) as f:
        head = f.read()
    with tempfile.TemporaryDirectory() as tmp:
        def image_of(tag):
            if a.earlier_images:
                p = os.path.join(a.earlier_images, tag + ".img")
                if not os.path.exists(p):
                    return None
                with open(p, "rb") as f:
                    return f.read()
            return gh_image(a.gh_repo, tag, tmp)
        try:
            lines = check(image, a.tag, head, release_tags(a.repo),
                          lambda t: git(a.repo, "show", f"{t}:src/tuya.h"), image_of)
            more, warnings = budget(image, ota, os.path.basename(a.ztu_ota), a.accept_headroom)
        except (Refused, fw.Refused) as e:
            sys.exit(str(e))
    print("\n".join(lines + more))
    for w in warnings:
        print(f"warning: {w}", file=sys.stderr)


if __name__ == "__main__":
    main(sys.argv)
