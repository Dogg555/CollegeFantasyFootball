#!/usr/bin/env python3
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import sys
import unittest
from unittest import mock


SCRIPT_PATH = Path(__file__).with_name("ops_ingest.py")
REPO_ROOT = SCRIPT_PATH.parent.parent
SPEC = importlib.util.spec_from_file_location("ops_ingest", SCRIPT_PATH)
OPS_INGEST = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(OPS_INGEST)


class OpsIngestTests(unittest.TestCase):
    def setUp(self):
        self.original_base_url = OPS_INGEST.BASE_URL
        self.original_admin_token = OPS_INGEST.ADMIN_TOKEN
        OPS_INGEST.BASE_URL = "http://college-ff-api:10000"
        OPS_INGEST.ADMIN_TOKEN = "test-admin-token"

    def tearDown(self):
        OPS_INGEST.BASE_URL = self.original_base_url
        OPS_INGEST.ADMIN_TOKEN = self.original_admin_token

    def run_main(self, responses, argv=None):
        calls = []

        def fake_request(method, path, admin=False, timeout=120):
            calls.append((method, path, admin, timeout))
            response = responses[(method, path)]
            if isinstance(response, Exception):
                raise response
            return response

        stdout = io.StringIO()
        with mock.patch.object(OPS_INGEST, "request", side_effect=fake_request), \
             mock.patch.object(OPS_INGEST.time, "sleep"), \
             mock.patch.object(sys, "argv", [str(SCRIPT_PATH), *(argv or ["--run"])]), \
             mock.patch.dict(os.environ, {
                 "CFF_INGEST_TIMEOUT_SECONDS": "900",
                 "CFF_INGEST_HEALTH_RETRIES": "2",
                 "CFF_INGEST_RETRY_DELAY_SECONDS": "0.1",
             }, clear=False), \
             contextlib.redirect_stdout(stdout):
            OPS_INGEST.main()
        return calls, stdout.getvalue()

    def test_successful_daily_run_checks_health_ingests_and_reads_status(self):
        calls, output = self.run_main({
            ("GET", "/health"): {"status": "ok", "database": "ok"},
            ("GET", "/api/health"): {"status": "ok", "database": "ok"},
            ("POST", "/api/admin/ingest/cfbd"): {"status": "ok", "ingested": 100},
            ("GET", "/api/admin/ingest/cfbd/status"): {"status": "completed"},
        })

        self.assertIn('"ingested": 100', output)
        self.assertIn(("POST", "/api/admin/ingest/cfbd", True, 900), calls)
        self.assertIn(("GET", "/api/admin/ingest/cfbd/status", True, 30), calls)

    def test_partial_ingest_fails_the_cron_run(self):
        with self.assertRaisesRegex(RuntimeError, "did not complete successfully"):
            self.run_main({
                ("GET", "/health"): {"status": "ok", "database": "ok"},
                ("GET", "/api/health"): {"status": "ok", "database": "ok"},
                ("POST", "/api/admin/ingest/cfbd"): {
                    "status": "partial",
                    "errors": ["provider request failed"],
                },
                ("GET", "/api/admin/ingest/cfbd/status"): {"status": "partial"},
            })

    def test_allow_partial_supports_manual_diagnostics(self):
        calls, output = self.run_main({
            ("GET", "/health"): {"status": "ok", "database": "ok"},
            ("GET", "/api/health"): {"status": "ok", "database": "ok"},
            ("POST", "/api/admin/ingest/cfbd"): {"status": "partial", "errors": ["one roster failed"]},
            ("GET", "/api/admin/ingest/cfbd/status"): {"status": "partial"},
        }, argv=["--run", "--allow-partial"])

        self.assertIn('"status": "partial"', output)
        self.assertEqual(sum(1 for call in calls if call[0] == "POST"), 1)

    def test_player_refresh_uses_current_fbs_roster_endpoints(self):
        source = (REPO_ROOT / "backend" / "src" / "cfbd_ingest.cpp").read_text(encoding="utf-8")
        self.assertIn('baseUrl + "/teams/fbs"', source)
        self.assertIn('normalizedBase + "/roster"', source)
        self.assertNotIn('normalizedBase + "/players"', source)
        self.assertIn("stale players were not retired", source)

        migration = (REPO_ROOT / "backend" / "db" / "migrations" / "004_current_roster_players.sql").read_text(encoding="utf-8")
        self.assertIn("active BOOLEAN", migration)
        self.assertIn("last_seen_at", migration)

    def test_live_cache_combines_schedule_and_scoreboard(self):
        source = (REPO_ROOT / "backend" / "src" / "live_scores.cpp").read_text(encoding="utf-8")
        self.assertIn('baseUrl + "/scoreboard"', source)
        self.assertIn('baseUrl + "/games"', source)
        self.assertIn("mergeGames", source)
        self.assertIn("refreshHours", source)

    def test_scoreboard_groups_games_by_week_and_kickoff(self):
        completed = subprocess.run(
            ["node", str(REPO_ROOT / "scripts" / "scoreboard_ui_tests.js")],
            cwd=REPO_ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr or completed.stdout)
        self.assertIn("scoreboard UI tests passed", completed.stdout)


if __name__ == "__main__":
    unittest.main()
