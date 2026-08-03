#!/usr/bin/env python3
"""Fix the remaining administrator email normalization call after auth extraction."""

from pathlib import Path

path = Path("backend/src/main.cpp")
text = path.read_text(encoding="utf-8")
old = "admins.find(lowerAscii(*email))"
new = "admins.find(canonicalEmail(*email))"
count = text.count(old)
if count != 1:
    raise SystemExit(f"expected one administrator normalization call, found {count}")
path.write_text(text.replace(old, new, 1), encoding="utf-8")
print("administrator email normalization now uses auth core")
