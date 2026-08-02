#!/usr/bin/env python3
"""Shared helpers for deployed release-gate validation scripts."""
from __future__ import annotations

import json
import os
import re
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable


class GateFailure(RuntimeError):
    """Raised when a mandatory release gate fails."""


def env_flag(name: str, default: bool = False) -> bool:
    raw = os.environ.get(name)
    if raw is None:
        return default
    return raw.strip().lower() in {"1", "true", "yes", "on"}


def require_env(name: str) -> str:
    value = os.environ.get(name, "").strip()
    if not value:
        raise GateFailure(f"Missing required environment variable: {name}")
    return value


def int_env(name: str, default: int, minimum: int | None = None, maximum: int | None = None) -> int:
    raw = os.environ.get(name, "").strip()
    try:
        value = int(raw) if raw else default
    except ValueError as exc:
        raise GateFailure(f"{name} must be an integer") from exc
    if minimum is not None and value < minimum:
        raise GateFailure(f"{name} must be at least {minimum}")
    if maximum is not None and value > maximum:
        raise GateFailure(f"{name} must be at most {maximum}")
    return value


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def mask_email(value: str) -> str:
    if "@" not in value:
        return "***"
    local, domain = value.rsplit("@", 1)
    visible = local[:2]
    return f"{visible}{'*' * max(3, len(local) - len(visible))}@{domain}"


def redact_url(value: str) -> str:
    try:
        parsed = urllib.parse.urlsplit(value)
    except ValueError:
        return "[invalid-url]"
    query = urllib.parse.parse_qsl(parsed.query, keep_blank_values=True)
    redacted = [(key, "[redacted]" if "token" in key.lower() else item) for key, item in query]
    return urllib.parse.urlunsplit((parsed.scheme, parsed.netloc, parsed.path, urllib.parse.urlencode(redacted), parsed.fragment))


def safe_filename(value: str) -> str:
    value = re.sub(r"[^A-Za-z0-9_.-]+", "-", value.strip())
    return value.strip("-.") or "artifact"


@dataclass
class HttpResult:
    status: int
    payload: Any
    headers: dict[str, str]
    text: str


class JsonHttpClient:
    def __init__(self, base_url: str, timeout: int = 45) -> None:
        self.base_url = base_url.strip().rstrip("/")
        if not self.base_url.startswith(("http://", "https://")):
            raise GateFailure("Base URL must begin with http:// or https://")
        self.timeout = timeout

    def request(
        self,
        method: str,
        path: str,
        *,
        body: Any | None = None,
        token: str | None = None,
        expected: Iterable[int] = (200,),
        headers: dict[str, str] | None = None,
    ) -> HttpResult:
        request_headers = {"Accept": "application/json", "User-Agent": "college-ff-release-gates/1.0"}
        if headers:
            request_headers.update(headers)
        data = None
        if body is not None:
            data = json.dumps(body).encode("utf-8")
            request_headers["Content-Type"] = "application/json"
        if token:
            request_headers["Authorization"] = f"Bearer {token}"
        request = urllib.request.Request(
            f"{self.base_url}{path}",
            data=data,
            headers=request_headers,
            method=method,
        )
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                raw = response.read().decode("utf-8", errors="replace")
                status = response.getcode()
                response_headers = {key.lower(): value for key, value in response.headers.items()}
        except urllib.error.HTTPError as exc:
            raw = exc.read().decode("utf-8", errors="replace")
            status = exc.code
            response_headers = {key.lower(): value for key, value in exc.headers.items()}
        except urllib.error.URLError as exc:
            raise GateFailure(f"{method} {path} could not connect to {self.base_url}: {exc}") from exc

        payload: Any = {}
        if raw:
            try:
                payload = json.loads(raw)
            except json.JSONDecodeError:
                payload = {"raw": raw}
        expected_set = set(expected)
        if status not in expected_set:
            raise GateFailure(f"{method} {path} expected {sorted(expected_set)}, got {status}: {payload}")
        return HttpResult(status=status, payload=payload, headers=response_headers, text=raw)


def write_report(report: dict[str, Any], directory: str | Path, stem: str) -> tuple[Path, Path]:
    output = Path(directory)
    output.mkdir(parents=True, exist_ok=True)
    stem = safe_filename(stem)
    json_path = output / f"{stem}.json"
    md_path = output / f"{stem}.md"
    json_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    lines = [f"# {report.get('title', stem)}", ""]
    classification = report.get("classification") or report.get("status")
    if classification:
        lines.extend([f"**Result:** `{classification}`", ""])
    if report.get("generatedAt"):
        lines.extend([f"Generated: `{report['generatedAt']}`", ""])
    checks = report.get("checks")
    if isinstance(checks, list):
        lines.extend(["| Check | Result | Detail |", "|---|---|---|"])
        for check in checks:
            name = str(check.get("name", "Unnamed")).replace("|", "\\|")
            passed = check.get("passed")
            required = check.get("required", True)
            result = "PASS" if passed else ("FAIL" if required else "WARN")
            detail = str(check.get("detail", "")).replace("|", "\\|").replace("\n", " ")
            lines.append(f"| {name} | {result} | {detail} |")
        lines.append("")
    summary = report.get("summary")
    if summary:
        lines.extend(["## Summary", "", str(summary), ""])
    md_path.write_text("\n".join(lines), encoding="utf-8")
    return json_path, md_path


def add_check(checks: list[dict[str, Any]], name: str, passed: bool, detail: str, required: bool = True) -> None:
    checks.append({"name": name, "passed": bool(passed), "required": bool(required), "detail": detail})


def enforce_checks(checks: list[dict[str, Any]]) -> None:
    failures = [check for check in checks if check.get("required", True) and not check.get("passed")]
    if failures:
        names = ", ".join(str(check.get("name", "unnamed")) for check in failures)
        raise GateFailure(f"Mandatory release gates failed: {names}")


def write_failure_report(title: str, stem: str, error: Exception | str, directory: str | Path | None = None) -> tuple[Path, Path]:
    """Persist a sanitized failure artifact when a gate exits before its full report is built."""
    output = Path(directory or os.environ.get("CFF_RELEASE_GATE_REPORT_DIR", "release-gate-artifacts"))
    existing_json = output / f"{safe_filename(stem)}.json"
    existing_md = output / f"{safe_filename(stem)}.md"
    if existing_json.exists():
        return existing_json, existing_md
    report = {
        "title": title,
        "generatedAt": utc_now(),
        "status": "failed",
        "classification": "gate-error",
        "checks": [
            {
                "name": "Gate execution",
                "passed": False,
                "required": True,
                "detail": str(error),
            }
        ],
        "summary": "The gate exited before completing its normal evidence report. Review the workflow logs and error detail.",
    }
    return write_report(report, output, stem)
