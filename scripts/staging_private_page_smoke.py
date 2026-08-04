#!/usr/bin/env python3
"""Exercise the deployed Players, League, and Draft pages with a real staging session."""

from __future__ import annotations

import json
import os
import pathlib
import time
import urllib.error
import urllib.request
from typing import Any

from playwright.sync_api import Page, sync_playwright


FRONTEND_BASE = os.environ.get(
    "CFF_INCIDENT_FRONTEND_BASE", "https://college-ff-staging-frontend.onrender.com"
).rstrip("/")
API_ORIGIN = os.environ.get(
    "CFF_INCIDENT_API_ORIGIN", "https://college-ff-staging-api.onrender.com"
).rstrip("/")
ARTIFACT_DIR = pathlib.Path(os.environ.get("CFF_INCIDENT_ARTIFACT_DIR", "staging-incident-artifacts"))


class SmokeFailure(RuntimeError):
    pass


def request(method: str, path: str, body: Any = None, token: str = "", expected=(200,)) -> Any:
    data = None
    headers = {"Accept": "application/json", "User-Agent": "cff-staging-incident-smoke/1.0"}
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    if token:
        headers["Authorization"] = f"Bearer {token}"
    url = f"{API_ORIGIN}{path}"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=60) as response:
            status = response.status
            text = response.read().decode("utf-8")
    except urllib.error.HTTPError as exc:
        status = exc.code
        text = exc.read().decode("utf-8")
    except urllib.error.URLError as exc:
        raise SmokeFailure(f"{method} {url} could not connect: {exc}") from exc
    if status not in expected:
        raise SmokeFailure(f"{method} {url} expected {expected}, got {status}: {text[:1000]}")
    if not text:
        return {}
    try:
        return json.loads(text)
    except json.JSONDecodeError as exc:
        raise SmokeFailure(f"{method} {url} returned invalid JSON: {text[:1000]}") from exc


def wait_for_api(timeout_seconds: int = 420) -> None:
    deadline = time.monotonic() + timeout_seconds
    last_error = ""
    while time.monotonic() < deadline:
        try:
            health = request("GET", "/health")
            if health.get("status") == "ok" and health.get("database") == "ok":
                return
            last_error = json.dumps(health)
        except Exception as exc:
            last_error = str(exc)
        time.sleep(10)
    raise SmokeFailure(f"staging API did not become healthy: {last_error}")


def create_session() -> tuple[str, str, dict[str, Any]]:
    suffix = str(int(time.time() * 1000))
    email = f"page-smoke+{suffix}@example.com"
    password = f"StagingSmoke!{suffix[-8:]}Aa"
    signup = request(
        "POST",
        "/api/auth/signup",
        {"email": email, "password": password},
        expected=(201,),
    )
    token = str(signup.get("token") or "")
    if not token:
        login = request("POST", "/api/auth/login", {"email": email, "password": password})
        token = str(login.get("token") or "")
    if not token:
        raise SmokeFailure(f"staging signup/login did not return a token: {signup}")
    validated = request("GET", "/api/auth/validate", token=token)
    if validated.get("valid") is not True:
        raise SmokeFailure(f"new staging token did not validate: {validated}")

    league = request(
        "POST",
        "/api/leagues",
        {
            "name": f"Page Smoke {suffix[-6:]}",
            "teams": 4,
            "scoring": "ppr",
            "draftType": "snake",
            "invitedEmails": [],
            "rosterRules": {"qb": 1, "rb": 1, "wr": 1, "te": 0, "flex": 0, "bench": 2},
        },
        token=token,
        expected=(201,),
    )
    if not league.get("id"):
        raise SmokeFailure(f"staging league creation returned no ID: {league}")
    return email, token, league


def install_session(page: Page, email: str, token: str) -> None:
    page.add_init_script(
        """
        ({ email, token }) => {
          sessionStorage.setItem('cff_auth', JSON.stringify({ email, token }));
          localStorage.removeItem('cff_auth');
        }
        """,
        {"email": email, "token": token},
    )


def check_page(page: Page, path: str, expected_selector: str, expected_text: str = "") -> dict[str, Any]:
    console_errors: list[str] = []
    page_errors: list[str] = []
    failed_requests: list[str] = []
    page.on("console", lambda message: console_errors.append(message.text) if message.type == "error" else None)
    page.on("pageerror", lambda error: page_errors.append(str(error)))
    page.on("requestfailed", lambda request: failed_requests.append(f"{request.method} {request.url}: {request.failure}"))

    response = page.goto(f"{FRONTEND_BASE}/{path}", wait_until="domcontentloaded", timeout=90000)
    page.wait_for_timeout(5000)
    if response is None or response.status >= 400:
        raise SmokeFailure(f"{path} failed to load: {response.status if response else 'no response'}")
    if page.url.startswith(f"{FRONTEND_BASE}/signin.html"):
        raise SmokeFailure(f"{path} unexpectedly redirected to sign in")
    locator = page.locator(expected_selector)
    locator.wait_for(state="attached", timeout=30000)
    text = locator.inner_text().strip()
    if expected_text and expected_text.lower() not in text.lower():
        raise SmokeFailure(f"{path} expected {expected_selector} to contain {expected_text!r}, got {text!r}")
    if page_errors:
        raise SmokeFailure(f"{path} page errors: {page_errors}")
    if console_errors:
        raise SmokeFailure(f"{path} console errors: {console_errors}")
    relevant_failures = [item for item in failed_requests if FRONTEND_BASE in item or API_ORIGIN in item]
    if relevant_failures:
        raise SmokeFailure(f"{path} failed requests: {relevant_failures}")

    screenshot = ARTIFACT_DIR / f"{path.split('?')[0].replace('.html', '')}.png"
    page.screenshot(path=str(screenshot), full_page=True)
    return {
        "path": path,
        "url": page.url,
        "status": response.status,
        "selector": expected_selector,
        "text": text,
        "consoleErrors": console_errors,
        "pageErrors": page_errors,
        "failedRequests": failed_requests,
        "screenshot": str(screenshot),
    }


def main() -> None:
    ARTIFACT_DIR.mkdir(parents=True, exist_ok=True)
    wait_for_api()

    auth_status = request("GET", "/api/auth/status")
    player_meta = request("GET", "/api/players/meta")
    players = request("GET", "/api/players?limit=5&offset=0")
    if not isinstance(players, list) or not players:
        raise SmokeFailure(f"public player endpoint returned no players: {players}")

    email, token, league = create_session()
    league_id = str(league["id"])
    results: list[dict[str, Any]] = []

    with sync_playwright() as playwright:
        browser = playwright.chromium.launch()
        try:
            context = browser.new_context(viewport={"width": 390, "height": 844})
            try:
                page = context.new_page()
                install_session(page, email, token)
                results.append(check_page(page, "players.html", "#search-results"))
                page.close()

                page = context.new_page()
                install_session(page, email, token)
                results.append(check_page(page, "league.html", "#league-name", league["name"]))
                page.close()

                page = context.new_page()
                install_session(page, email, token)
                results.append(check_page(page, f"draft.html?league={league_id}", "#draft-league-name", league["name"]))
                page.close()
            finally:
                context.close()
        finally:
            browser.close()

    report = {
        "status": "passed",
        "frontend": FRONTEND_BASE,
        "api": API_ORIGIN,
        "authStatus": auth_status,
        "playerMeta": player_meta,
        "testEmail": email,
        "leagueId": league_id,
        "results": results,
    }
    (ARTIFACT_DIR / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    try:
        main()
    except SmokeFailure as exc:
        ARTIFACT_DIR.mkdir(parents=True, exist_ok=True)
        failure = {"status": "failed", "error": str(exc), "frontend": FRONTEND_BASE, "api": API_ORIGIN}
        (ARTIFACT_DIR / "report.json").write_text(json.dumps(failure, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(failure, indent=2))
        raise SystemExit(1)
