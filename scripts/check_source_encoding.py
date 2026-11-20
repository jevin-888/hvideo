#!/usr/bin/env python3
"""Reject invalid UTF-8 and obvious damaged text before packaging application code."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOTS = ("src", "include", "app/src/main/java", "app/src/main/res")
EXTENSIONS = {".cpp", ".h", ".hpp", ".c", ".java", ".kt", ".js", ".html", ".css", ".xml", ".json", ".dart"}
BROKEN_TEXT = re.compile(r"\?{3,}|([\"'`])\?{2}\1|\uFFFD")


def check_text(text):
    return [number for number, line in enumerate(text.splitlines(), 1) if BROKEN_TEXT.search(line)]


def main():
    errors = []
    count = 0
    for directory in SOURCE_ROOTS:
        for path in sorted((ROOT / directory).rglob("*")):
            if not path.is_file() or path.suffix.lower() not in EXTENSIONS:
                continue
            count += 1
            try:
                text = path.read_text(encoding="utf-8-sig", errors="strict")
            except UnicodeDecodeError:
                errors.append(f"{path.relative_to(ROOT)}: invalid UTF-8")
                continue
            # Vendored minified libraries contain intentional replacement tables.
            # Still enforce valid UTF-8, without applying our UI-text heuristic.
            vendor_xlsx = path == ROOT / 'src/network/web/libs/xlsx.full.min.js'
            for line in ([] if vendor_xlsx else check_text(text)):
                errors.append(f"{path.relative_to(ROOT)}:{line}: damaged question-mark/replacement text")
    if errors:
        print("Source encoding check failed:\n" + "\n".join(errors))
        return 1
    print(f"Source encoding check passed: {count} application source files.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
