#!/usr/bin/env python3
"""Report or remove safe, redundant includes in engine C++ sources.

Run from any directory. Pass --apply to write changes; the default is a dry run.
Headers keep their own dependencies, and conditional includes are left alone.
"""

import argparse
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
ENGINE = ROOT / "Engine"
PCH = ENGINE / "pch.h"
INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]\s*$')
DIRECTIVE = re.compile(r'^\s*#\s*(if|ifdef|ifndef|endif)\b')


def read_lines(path):
    # splitlines(keepends=True) retains each file's existing line endings.
    with path.open("r", encoding="utf-8", newline="") as source:
        return source.read().splitlines(keepends=True)


def directive(line, depth):
    match = DIRECTIVE.match(line)
    if match:
        if match.group(1) == "endif":
            return max(0, depth - 1)
        return depth + 1
    return depth


def clean_pch(lines):
    # pch.h wraps its C++ contents in #ifdef __cplusplus. Only consider its
    # standard headers at that depth, never platform-specific nested branches.
    depth = 0
    in_cpp = False
    seen = set()
    result = []
    removed = []
    for line in lines:
        if depth == 0 and re.match(r'^\s*#\s*ifdef\s+__cplusplus\b', line):
            in_cpp = True
        match = INCLUDE.match(line.rstrip("\r\n"))
        if in_cpp and depth == 1 and match and match.group(1) == "<":
            name = match.group(2)
            # Standard-library headers have no path or .h suffix.
            if "/" not in name and not name.endswith(".h"):
                if name in seen:
                    removed.append(name)
                    continue
                seen.add(name)
        result.append(line)
        depth = directive(line, depth)
        if in_cpp and depth == 0:
            in_cpp = False
    return result, removed, seen


def clean_source(lines, pch_headers):
    depth = 0
    has_pch = False
    seen = set()
    result = []
    removed = []
    for line in lines:
        match = INCLUDE.match(line.rstrip("\r\n"))
        if depth == 0 and match:
            delimiter, name = match.groups()
            if delimiter == '"' and name == "pch.h":
                has_pch = True
            key = (delimiter, name)
            standard = delimiter == "<" and "/" not in name and not name.endswith(".h")
            if standard and (key in seen or (has_pch and name in pch_headers)):
                removed.append(name)
                continue
            seen.add(key)
        result.append(line)
        depth = directive(line, depth)
    return result, removed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apply", action="store_true", help="write changes")
    args = parser.parse_args()
    pch_lines, removed, pch_headers = clean_pch(read_lines(PCH))
    changes = [(PCH, pch_lines, removed)] if removed else []
    for path in sorted(ENGINE.rglob("*.cpp")):
        lines, removed = clean_source(read_lines(path), pch_headers)
        if removed:
            changes.append((path, lines, removed))
    for path, lines, removed in changes:
        print(f"{path.relative_to(ROOT)}: {', '.join(removed)}")
        if args.apply:
            with path.open("w", encoding="utf-8", newline="") as output:
                output.writelines(lines)
    verb = "Updated" if args.apply else "Would update"
    print(f"{verb} {len(changes)} files."
          + ("" if args.apply else " Run with --apply to write changes."))


if __name__ == "__main__":
    main()
