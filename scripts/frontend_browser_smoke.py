#!/usr/bin/env python3
"""Desktop and mobile browser smoke tests for the deployed static frontend."""

from __future__ import annotations

import json
import os
import pathlib
import re
import sys
import time
from typing import Any
from urllib.parse import urljoin
from urllib.request import urlopen

from playwright.sync_api import sync_playwright


BASE_URL = os.environ.get("CFF_FRONTEND_BASE_URL", "").strip().rstrip("/") + "/"
EXPECTED_COMMIT = os.environ.get("RENDER_COMMIT_SHA", "").strip()
ARTIFACT_DIR = pathlib.Path(os.environ.get("CFF_BROWSER_ARTIFACT_DIR", "browser-artifacts"))
REPORT_DIR = pathlib.Path(os.environ.get("CFF_RELEASE_GATE_REPORT_DIR", "release-gate-artifacts"))
ALLOWED_CONSOLE_ERROR_REGEX = os.environ.get("CFF_BROWSER_ALLOWED_CONSOLE_ERROR_REGEX", "").strip()
PAGES = [
    "index.html",
    "signin.html",
    "signup.html",
    "verify-email.html",
    "resend-verification.html",
    "reset-request.html",
    "reset-password.html",
    "league.html",
    "players.html",
]
VIEWPORTS = {
    "desktop": {"width": 1440, "height": 900},
    "mobile": {"width": 390, "height": 844},
}


class BrowserFailure(RuntimeError):
    pass


def wait_for_frontend_commit(timeout_seconds: int = 600) -> dict[str, Any] | None:
    if not EXPECTED_COMMIT:
        return None
    deadline = time.monotonic() + timeout_seconds
    last: Any = None
    while time.monotonic() < deadline:
        try:
            with urlopen(urljoin(BASE_URL, "build-info.json"), timeout=30) as response:
                payload = json.load(response)
            last = str(payload.get("commit", ""))
            if last and (EXPECTED_COMMIT.startswith(last) or last.startswith(EXPECTED_COMMIT)):
                return payload
        except Exception as exc:
            last = str(exc)
        time.sleep(10)
    raise BrowserFailure(
        f"frontend never reported expected commit {EXPECTED_COMMIT}; "
        f"last build-info result was {last}"
    )


def allowed_console_error(message: str) -> bool:
    return bool(
        ALLOWED_CONSOLE_ERROR_REGEX
        and re.search(ALLOWED_CONSOLE_ERROR_REGEX, message, flags=re.IGNORECASE)
    )


def write_evidence(
    *,
    status: str,
    build_info: dict[str, Any] | None,
    results: list[dict[str, Any]],
    failures: list[str],
) -> None:
    ARTIFACT_DIR.mkdir(parents=True, exist_ok=True)
    REPORT_DIR.mkdir(parents=True, exist_ok=True)
    payload = {
        "status": status,
        "baseUrl": BASE_URL,
        "expectedCommit": EXPECTED_COMMIT or None,
        "build": build_info,
        "viewports": VIEWPORTS,
        "pages": PAGES,
        "results": results,
        "failures": failures,
    }
    encoded = json.dumps(payload, indent=2, sort_keys=True) + "\n"
    (ARTIFACT_DIR / "browser-validation.json").write_text(encoded, encoding="utf-8")
    (REPORT_DIR / "browser-validation.json").write_text(encoded, encoding="utf-8")

    checked = len(results)
    console_error_count = sum(len(result.get("blockingConsoleErrors", [])) for result in results)
    markdown = "\n".join(
        [
            "# Desktop and mobile browser validation",
            "",
            f"- Status: {status}",
            f"- Frontend: {BASE_URL}",
            f"- Expected commit: {EXPECTED_COMMIT or 'not required'}",
            f"- Page/viewport checks completed: {checked}",
            f"- Blocking console or page errors: {console_error_count}",
            f"- Failures: {len(failures)}",
            "",
        ]
        + [f"- {failure}" for failure in failures]
        + [""]
    )
    (REPORT_DIR / "browser-validation.md").write_text(markdown, encoding="utf-8")

    step_summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if step_summary:
        with open(step_summary, "a", encoding="utf-8") as handle:
            handle.write("\n" + markdown)


def main() -> None:
    if BASE_URL == "/":
        raise BrowserFailure("CFF_FRONTEND_BASE_URL is required")

    ARTIFACT_DIR.mkdir(parents=True, exist_ok=True)
    build_info = wait_for_frontend_commit()
    results: list[dict[str, Any]] = []
    failures: list[str] = []

    with sync_playwright() as playwright:
        browser = playwright.chromium.launch()
        try:
            for viewport_name, viewport in VIEWPORTS.items():
                context = browser.new_context(viewport=viewport)
                try:
                    for page_name in PAGES:
                        page = context.new_page()
                        observed_errors: list[str] = []
                        response_status: int | None = None
                        screenshot = ARTIFACT_DIR / (
                            f"{viewport_name}-{page_name.replace('.html', '')}.png"
                        )
                        try:
                            page.on(
                                "pageerror",
                                lambda exc, errors=observed_errors: errors.append(
                                    f"pageerror: {exc}"
                                ),
                            )
                            page.on(
                                "console",
                                lambda message, errors=observed_errors: errors.append(
                                    f"console: {message.text}"
                                )
                                if message.type == "error"
                                else None,
                            )
                            response = page.goto(
                                urljoin(BASE_URL, page_name),
                                wait_until="domcontentloaded",
                                timeout=60000,
                            )
                            page.wait_for_timeout(1000)
                            response_status = response.status if response else None
                            if response is None or response.status >= 400:
                                raise BrowserFailure(
                                    f"failed to load: "
                                    f"{response.status if response else 'no response'}"
                                )
                            page.locator("body").wait_for(state="visible")
                            overflow = page.evaluate(
                                "Math.max(document.documentElement.scrollWidth, "
                                "document.body.scrollWidth) - window.innerWidth"
                            )
                            if overflow > 4:
                                raise BrowserFailure(
                                    f"has {overflow}px horizontal overflow"
                                )

                            if viewport_name == "mobile" and page_name == "index.html":
                                toggle = page.locator(".nav-toggle")
                                if not toggle.is_visible():
                                    raise BrowserFailure(
                                        "mobile navigation toggle is not visible"
                                    )
                                toggle.click()
                                if toggle.get_attribute("aria-expanded") != "true":
                                    raise BrowserFailure(
                                        "mobile navigation toggle did not open"
                                    )
                                toggle.click()

                            if page_name in {"signup.html", "reset-password.html"}:
                                password = page.locator('input[type="password"]').first
                                if password.count() and (
                                    password.get_attribute("minlength") != "12"
                                    or password.get_attribute("maxlength") != "72"
                                ):
                                    raise BrowserFailure(
                                        f"{page_name} password constraints "
                                        "do not match the API policy"
                                    )
                        except Exception as exc:
                            failures.append(f"{viewport_name} {page_name}: {exc}")
                        finally:
                            try:
                                page.screenshot(path=str(screenshot), full_page=True)
                            except Exception as exc:
                                failures.append(
                                    f"{viewport_name} {page_name}: "
                                    f"screenshot failed: {exc}"
                                )

                            blocking_errors = [
                                error
                                for error in observed_errors
                                if not allowed_console_error(error)
                            ]
                            if blocking_errors:
                                failures.append(
                                    f"{viewport_name} {page_name}: "
                                    f"{len(blocking_errors)} blocking browser error(s)"
                                )

                            results.append(
                                {
                                    "viewport": viewport_name,
                                    "page": page_name,
                                    "responseStatus": response_status,
                                    "screenshot": str(screenshot),
                                    "consoleErrors": observed_errors,
                                    "blockingConsoleErrors": blocking_errors,
                                }
                            )
                            page.close()
                finally:
                    context.close()
        finally:
            browser.close()

    status = "passed" if not failures else "failed"
    write_evidence(
        status=status,
        build_info=build_info,
        results=results,
        failures=failures,
    )
    if failures:
        raise BrowserFailure(
            f"{len(failures)} browser validation failure(s); "
            "see browser-validation.json"
        )

    print(
        json.dumps(
            {
                "status": "ok",
                "baseUrl": BASE_URL,
                "build": build_info,
                "results": results,
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    try:
        main()
    except BrowserFailure as exc:
        if not (REPORT_DIR / "browser-validation.json").exists():
            write_evidence(
                status="failed",
                build_info=None,
                results=[],
                failures=[str(exc)],
            )
        print(
            json.dumps({"status": "failed", "error": str(exc)}, indent=2),
            file=sys.stderr,
        )
        raise SystemExit(1)
