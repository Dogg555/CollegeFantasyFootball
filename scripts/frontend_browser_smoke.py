#!/usr/bin/env python3
"""Desktop and mobile browser smoke tests for the deployed static frontend."""

import json
import os
import pathlib
import sys
import time
from urllib.parse import urljoin
from urllib.request import urlopen

from playwright.sync_api import sync_playwright


BASE_URL = os.environ.get("CFF_FRONTEND_BASE_URL", "").strip().rstrip("/") + "/"
EXPECTED_COMMIT = os.environ.get("RENDER_COMMIT_SHA", "").strip()
ARTIFACT_DIR = pathlib.Path("browser-artifacts")
PAGES = ["index.html", "signin.html", "signup.html", "reset-password.html", "league.html", "players.html"]
VIEWPORTS = {
    "desktop": {"width": 1440, "height": 900},
    "mobile": {"width": 390, "height": 844},
}


class BrowserFailure(RuntimeError):
    pass


def wait_for_frontend_commit(timeout_seconds=600):
    if not EXPECTED_COMMIT:
        return None
    deadline = time.monotonic() + timeout_seconds
    last = None
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
        f"frontend never reported expected commit {EXPECTED_COMMIT}; last build-info result was {last}"
    )


def main():
    if BASE_URL == "/":
        raise BrowserFailure("CFF_FRONTEND_BASE_URL is required")
    ARTIFACT_DIR.mkdir(parents=True, exist_ok=True)
    build_info = wait_for_frontend_commit()
    results = []

    with sync_playwright() as playwright:
        browser = playwright.chromium.launch()
        for viewport_name, viewport in VIEWPORTS.items():
            context = browser.new_context(viewport=viewport)
            for page_name in PAGES:
                page = context.new_page()
                errors = []
                page.on("pageerror", lambda exc, errors=errors: errors.append(f"pageerror: {exc}"))
                page.on(
                    "console",
                    lambda message, errors=errors: errors.append(f"console: {message.text}")
                    if message.type == "error"
                    else None,
                )
                response = page.goto(urljoin(BASE_URL, page_name), wait_until="domcontentloaded", timeout=60000)
                page.wait_for_timeout(1000)
                if response is None or response.status >= 400:
                    raise BrowserFailure(
                        f"{viewport_name} {page_name} failed to load: {response.status if response else 'no response'}"
                    )
                page.locator("body").wait_for(state="visible")
                overflow = page.evaluate(
                    "Math.max(document.documentElement.scrollWidth, document.body.scrollWidth) - window.innerWidth"
                )
                if overflow > 4:
                    raise BrowserFailure(f"{viewport_name} {page_name} has {overflow}px horizontal overflow")

                if viewport_name == "mobile" and page_name == "index.html":
                    toggle = page.locator(".nav-toggle")
                    if not toggle.is_visible():
                        raise BrowserFailure("mobile navigation toggle is not visible")
                    toggle.click()
                    if toggle.get_attribute("aria-expanded") != "true":
                        raise BrowserFailure("mobile navigation toggle did not open")
                    toggle.click()

                if page_name in {"signup.html", "reset-password.html"}:
                    password = page.locator('input[type="password"]').first
                    if password.count() and (
                        password.get_attribute("minlength") != "12"
                        or password.get_attribute("maxlength") != "72"
                    ):
                        raise BrowserFailure(f"{page_name} password constraints do not match the API policy")

                screenshot = ARTIFACT_DIR / f"{viewport_name}-{page_name.replace('.html', '')}.png"
                page.screenshot(path=str(screenshot), full_page=True)
                results.append({
                    "viewport": viewport_name,
                    "page": page_name,
                    "screenshot": str(screenshot),
                    "consoleErrors": errors,
                })
                page.close()
            context.close()
        browser.close()

    print(json.dumps({"status": "ok", "baseUrl": BASE_URL, "build": build_info, "results": results}, indent=2))


if __name__ == "__main__":
    try:
        main()
    except BrowserFailure as exc:
        print(json.dumps({"status": "failed", "error": str(exc)}, indent=2), file=sys.stderr)
        raise SystemExit(1)
