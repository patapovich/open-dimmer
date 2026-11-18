#!/usr/bin/env python3
"""Print the CHANGELOG.md section of one version (the release notes).

Usage: tools/changelog.py X.Y.Z|vX.Y.Z [CHANGELOG.md]
Exits 1 (and prints nothing) when the version has no section or the section is empty."""
import os
import re
import sys


def section(text, version):
    """Body of the '## X.Y.Z (...)' section, without the heading and surrounding blank lines;
    a bullet wrapped over several lines becomes one line."""
    version = version.lstrip("v")
    lines, body, inside = text.splitlines(), [], False
    for line in lines:
        if line.startswith("## "):
            if inside:
                break
            inside = re.match(r"## +" + re.escape(version) + r"(\s|$)", line) is not None
            continue
        if inside:
            body.append(line)
    # One line per bullet: GitHub shows a line break inside a release note as a break.
    out = []
    for line in body:
        if line.startswith("  ") and line.strip() and out and out[-1].startswith("- "):
            out[-1] += " " + line.strip()
        else:
            out.append(line)
    return "\n".join(out).strip()


def main(argv):
    if len(argv) < 2:
        sys.exit("usage: changelog.py X.Y.Z [CHANGELOG.md]")
    path = argv[2] if len(argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                       "..", "CHANGELOG.md")
    with open(path, encoding="utf-8") as f:
        notes = section(f.read(), argv[1])
    if not notes:
        sys.exit(f"CHANGELOG.md has no entry for {argv[1].lstrip('v')}")
    print(notes)


if __name__ == "__main__":
    main(sys.argv)
