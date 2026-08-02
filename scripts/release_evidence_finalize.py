#!/usr/bin/env python3
"""Combine successful release-gate artifacts into one issue-ready evidence bundle."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
from typing import Any


class FinalizationFailure(RuntimeError):
    pass


ALPHA_DIR = Path(os.environ.get("CFF_FINALIZER_ALPHA_DIR", "alpha-source"))
BACKUP_DIR = Path(os.environ.get("CFF_FINALIZER_BACKUP_DIR", "backup-source"))
RENDER_DIR = Path(os.environ.get("CFF_FINALIZER_RENDER_DIR", "render-source"))
OUTPUT_DIR = Path(
    os.environ.get("CFF_FINALIZER_OUTPUT_DIR", "release-candidate-evidence")
)
DEPLOYED_COMMIT = os.environ.get("CFF_FINALIZER_DEPLOYED_COMMIT", "").strip()
ALPHA_RUN_ID = os.environ.get("CFF_FINALIZER_ALPHA_RUN_ID", "").strip()
BACKUP_RUN_ID = os.environ.get("CFF_FINALIZER_BACKUP_RUN_ID", "").strip()
RENDER_RUN_ID = os.environ.get("CFF_FINALIZER_RENDER_RUN_ID", "").strip()
EXPECTED_BACKUP_SHA256 = os.environ.get(
    "CFF_FINALIZER_BACKUP_SHA256", ""
).strip().lower()
REPOSITORY = os.environ.get("GITHUB_REPOSITORY", "").strip()
SERVER_URL = os.environ.get("GITHUB_SERVER_URL", "https://github.com").rstrip("/")

REQUIRED_SECRET_NAMES = [
    name.strip()
    for name in os.environ.get("CFF_FINALIZER_REQUIRED_SECRET_NAMES", "").split(",")
    if name.strip()
]

REQUIRED_REPORTS = {
    "transactionalEmail": "transactional-email-acceptance.json",
    "fullLifecycle": "full-lifecycle-validation.json",
    "browser": "browser-validation.json",
    "publicData": "public-data-meta-validation.json",
    "backupRestore": "backup-restore-validation.json",
}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def find_unique(root: Path, filename: str) -> Path:
    matches = list(root.rglob(filename))
    if len(matches) != 1:
        raise FinalizationFailure(
            f"Expected exactly one {filename} under {root}; found {len(matches)}"
        )
    return matches[0]


def load_report(root: Path, filename: str) -> tuple[Path, dict[str, Any]]:
    path = find_unique(root, filename)
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise FinalizationFailure(f"Unable to read {path}: {exc}") from exc
    if not isinstance(payload, dict):
        raise FinalizationFailure(f"{path} did not contain a JSON object")
    status = str(payload.get("status", "")).lower()
    if status not in {"passed", "ok"}:
        raise FinalizationFailure(f"{filename} status was {status or 'missing'}")
    return path, payload


def check_names(report: dict[str, Any]) -> set[str]:
    checks = report.get("checks", [])
    if not isinstance(checks, list):
        return set()
    return {
        str(check.get("name", ""))
        for check in checks
        if isinstance(check, dict) and check.get("passed") is True
    }


def require_check_names(
    report_name: str,
    report: dict[str, Any],
    required: set[str],
) -> None:
    present = check_names(report)
    missing = sorted(required - present)
    if missing:
        raise FinalizationFailure(
            f"{report_name} is missing passed checks: {', '.join(missing)}"
        )


def validate_inputs() -> None:
    if len(DEPLOYED_COMMIT) != 40 or any(
        character not in "0123456789abcdefABCDEF" for character in DEPLOYED_COMMIT
    ):
        raise FinalizationFailure(
            "CFF_FINALIZER_DEPLOYED_COMMIT must be an exact 40-character SHA"
        )
    for name, value in (
        ("CFF_FINALIZER_ALPHA_RUN_ID", ALPHA_RUN_ID),
        ("CFF_FINALIZER_BACKUP_RUN_ID", BACKUP_RUN_ID),
        ("CFF_FINALIZER_RENDER_RUN_ID", RENDER_RUN_ID),
    ):
        if not value.isdigit():
            raise FinalizationFailure(f"{name} must be numeric")
    if len(EXPECTED_BACKUP_SHA256) != 64 or any(
        character not in "0123456789abcdef" for character in EXPECTED_BACKUP_SHA256
    ):
        raise FinalizationFailure(
            "CFF_FINALIZER_BACKUP_SHA256 must be a lowercase 64-character digest"
        )
    if not REPOSITORY:
        raise FinalizationFailure("GITHUB_REPOSITORY is required")


def main() -> int:
    validate_inputs()
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    secret_inventory = {
        name: bool(os.environ.get(name, "").strip())
        for name in REQUIRED_SECRET_NAMES
    }
    missing_secrets = sorted(
        name for name, present in secret_inventory.items() if not present
    )
    if missing_secrets:
        raise FinalizationFailure(
            "Missing required GitHub Actions secrets: "
            + ", ".join(missing_secrets)
        )

    report_paths: dict[str, Path] = {}
    reports: dict[str, dict[str, Any]] = {}
    for key, filename in REQUIRED_REPORTS.items():
        if key == "backupRestore":
            root = BACKUP_DIR
        elif key == "browser":
            root = RENDER_DIR
        else:
            root = ALPHA_DIR
        path, payload = load_report(root, filename)
        report_paths[key] = path
        reports[key] = payload

    require_check_names(
        "transactional email",
        reports["transactionalEmail"],
        {
            "Verification email delivered",
            "Resent verification email delivered",
            "Verification link activates account",
            "Password-reset email delivered",
            "Password reset completed",
            "Password reset revokes sessions",
            "Reset token is single-use",
            "Password-reset enumeration protection",
        },
    )
    require_check_names(
        "full lifecycle",
        reports["fullLifecycle"],
        {
            "Three verified accounts can sign in",
            "Invites, joins, and approvals persist",
            "One-player snake draft completes",
            "Drafted rosters persisted",
            "Accepted trade swaps both rosters",
            "Commissioner waiver processing updates roster",
            "Season schedule generated",
            "Scoring calculation completed",
            "Scoring week finalized",
            "Final matchups produce standings evidence",
        },
    )

    browser = reports["browser"]
    if browser.get("failures"):
        raise FinalizationFailure("Browser report contains failures")
    viewports = {
        str(row.get("viewport"))
        for row in browser.get("results", [])
        if isinstance(row, dict)
    }
    if not {"desktop", "mobile"}.issubset(viewports):
        raise FinalizationFailure(
            "Browser evidence does not contain both desktop and mobile results"
        )
    if str(browser.get("expectedCommit", "")).lower() != DEPLOYED_COMMIT.lower():
        raise FinalizationFailure(
            "Browser evidence expectedCommit does not match the deployed commit"
        )

    backup_path = report_paths["backupRestore"]
    actual_backup_sha256 = sha256_file(backup_path)
    if actual_backup_sha256 != EXPECTED_BACKUP_SHA256:
        raise FinalizationFailure(
            "Backup report digest does not match the requested SHA-256"
        )

    copied_reports = {}
    for key, path in report_paths.items():
        destination = OUTPUT_DIR / path.name
        destination.write_bytes(path.read_bytes())
        copied_reports[key] = {
            "file": destination.name,
            "sha256": sha256_file(destination),
        }

    alpha_run_url = f"{SERVER_URL}/{REPOSITORY}/actions/runs/{ALPHA_RUN_ID}"
    backup_run_url = f"{SERVER_URL}/{REPOSITORY}/actions/runs/{BACKUP_RUN_ID}"
    render_run_url = f"{SERVER_URL}/{REPOSITORY}/actions/runs/{RENDER_RUN_ID}"
    manifest = {
        "status": "passed",
        "classification": "alpha-evidence-ready",
        "repository": REPOSITORY,
        "deployedCommit": DEPLOYED_COMMIT,
        "sourceRuns": {
            "alphaReadiness": {
                "runId": ALPHA_RUN_ID,
                "url": alpha_run_url,
            },
            "renderValidation": {
                "runId": RENDER_RUN_ID,
                "url": render_run_url,
            },
            "backupRestore": {
                "runId": BACKUP_RUN_ID,
                "url": backup_run_url,
                "evidenceSha256": EXPECTED_BACKUP_SHA256,
            },
        },
        "secretPresence": {
            name: {"present": present, "valueRetained": False}
            for name, present in secret_inventory.items()
        },
        "reports": copied_reports,
        "verifiedGates": [
            "transactional verification email delivery and resend",
            "password-reset delivery, completion, session revocation, and enumeration protection",
            "full three-account authentication and league lifecycle",
            "draft, roster, trade, waiver, schedule, scoring, finalization, and standings",
            "desktop and mobile browser validation against the exact deployed commit",
            "player and schedule metadata",
            "real off-platform backup restore into a disposable database",
        ],
    }
    manifest_path = OUTPUT_DIR / "release-candidate-evidence.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )

    markdown = "\n".join(
        [
            "# Release candidate evidence",
            "",
            "- Status: passed",
            f"- Exact deployed commit: `{DEPLOYED_COMMIT}`",
            f"- Alpha readiness run: {alpha_run_url}",
            f"- Render validation run: {render_run_url}",
            f"- Backup restore run: {backup_run_url}",
            f"- Backup evidence SHA-256: `{EXPECTED_BACKUP_SHA256}`",
            f"- Required GitHub Actions secrets present: {len(secret_inventory)}",
            "- Secret values retained: no",
            "",
            "## Verified gates",
            "",
        ]
        + [f"- {gate}" for gate in manifest["verifiedGates"]]
        + [""]
    )
    (OUTPUT_DIR / "release-candidate-evidence.md").write_text(
        markdown,
        encoding="utf-8",
    )

    step_summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if step_summary:
        with open(step_summary, "a", encoding="utf-8") as handle:
            handle.write(markdown)

    print(
        json.dumps(
            {
                "status": "ok",
                "manifest": str(manifest_path),
                "deployedCommit": DEPLOYED_COMMIT,
            },
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
        failure = {
            "status": "failed",
            "classification": "alpha-evidence-blocked",
            "error": str(exc),
        }
        (OUTPUT_DIR / "release-candidate-evidence.json").write_text(
            json.dumps(failure, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        print(f"Release evidence finalization failed: {exc}", file=os.sys.stderr)
        raise SystemExit(1)
