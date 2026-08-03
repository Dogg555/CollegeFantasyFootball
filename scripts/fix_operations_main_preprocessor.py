#!/usr/bin/env python3
"""Remove the stale PostgreSQL #endif left by the operations extraction."""

from pathlib import Path

path = Path("backend/src/main.cpp")
text = path.read_text(encoding="utf-8")
old = '''std::string jsonToString(const Json::Value &value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

#endif

std::string firstHeaderValue'''
new = '''std::string jsonToString(const Json::Value &value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

std::string firstHeaderValue'''
if text.count(old) != 1:
    raise SystemExit(f"expected one stale operations preprocessor boundary, found {text.count(old)}")
path.write_text(text.replace(old, new, 1), encoding="utf-8")
print("operations main preprocessor boundary corrected")
