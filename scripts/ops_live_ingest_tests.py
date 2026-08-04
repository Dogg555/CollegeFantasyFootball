#!/usr/bin/env python3
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import unittest
from unittest import mock


SCRIPT_PATH = Path(__file__).with_name("ops_live_ingest.py")
SPEC = importlib.util.spec_from_file_location("ops_live_ingest", SCRIPT_PATH)
OPS_LIVE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(OPS_LIVE)


class OpsLiveIngestTests(unittest.TestCase):
    def setUp(self):
        self.original_base_url = OPS_LIVE.BASE_URL
        self.original_admin_token = OPS_LIVE.ADMIN_TOKEN
        OPS_LIVE.BASE_URL = "http://college-ff-api:10000"
        OPS_LIVE.ADMIN_TOKEN = "test-admin-token"

    def tearDown(self):
        OPS_LIVE.BASE_URL = self.original_base_url
        OPS_LIVE.ADMIN_TOKEN = self.original_admin_token

    def run_main(self, responses):
        calls = []

        def fake_request(method, path, admin=False, timeout=60):
            calls.append((method, path, admin, timeout))
            if admin and not OPS_LIVE.ADMIN_TOKEN:
                raise RuntimeError("CFF_ADMIN_API_TOKEN is required")
            response = responses[(method, path)]
            if isinstance(response, Exception):
                raise response
            return response

        stdout = io.StringIO()
        with mock.patch.object(OPS_LIVE, "request", side_effect=fake_request), \
             mock.patch.object(OPS_LIVE.time, "sleep"), \
             mock.patch.dict(os.environ, {
                 "CFF_LIVE_INGEST_HEALTH_RETRIES": "2",
                 "CFF_LIVE_INGEST_RETRY_DELAY_SECONDS": "1",
                 "CFF_LIVE_INGEST_TIMEOUT_SECONDS": "90",
             }, clear=False), \
             contextlib.redirect_stdout(stdout):
            OPS_LIVE.main()
        return calls, stdout.getvalue()

    def test_successful_run_checks_health_ingests_and_reads_status(self):
        calls, output = self.run_main({
            ("GET", "/health"): {"status": "ok", "database": "ok"},
            ("GET", "/api/health"): {"status": "ok", "database": "ok"},
            ("POST", "/api/admin/live-stats/run"): {
                "status": "succeeded",
                "code": "ingest_started",
                "games": 22,
                "liveGames": 4,
                "apiCalls": 1,
            },
            ("GET", "/api/admin/live-stats/status"): {
                "status": "ok",
                "runs": [],
                "freshness": [],
            },
        })

        self.assertIn('"liveGames": 4', output)
        self.assertIn(("POST", "/api/admin/live-stats/run", True, 90), calls)
        self.assertIn(("GET", "/api/admin/live-stats/status", True, 30), calls)
        self.assertNotIn(
            ("POST", "/api/admin/ingest/cfbd/live", True, 90), calls
        )

    def test_recent_duplicate_is_a_successful_no_op(self):
        calls, output = self.run_main({
            ("GET", "/health"): {"status": "ok", "database": "ok"},
            ("GET", "/api/health"): {"status": "ok", "database": "ok"},
            ("POST", "/api/admin/live-stats/run"): {
                "status": "duplicate",
                "code": "duplicate_ingest",
                "accepted": False,
            },
            ("GET", "/api/admin/live-stats/status"): {"status": "ok"},
        })

        self.assertIn('"duplicate_ingest"', output)
        self.assertIn(("POST", "/api/admin/live-stats/run", True, 90), calls)

    def test_already_running_is_a_successful_no_op(self):
        calls, output = self.run_main({
            ("GET", "/health"): {"status": "ok", "database": "ok"},
            ("GET", "/api/health"): {"status": "ok", "database": "ok"},
            ("POST", "/api/admin/live-stats/run"): {
                "status": "skipped",
                "code": "ingest_already_running",
                "accepted": False,
            },
            ("GET", "/api/admin/live-stats/status"): {"status": "ok"},
        })

        self.assertIn('"ingest_already_running"', output)
        self.assertIn(("POST", "/api/admin/live-stats/run", True, 90), calls)

    def test_partial_result_fails_cron(self):
        with self.assertRaisesRegex(RuntimeError, "did not complete successfully"):
            self.run_main({
                ("GET", "/health"): {"status": "ok", "database": "ok"},
                ("GET", "/api/health"): {"status": "ok", "database": "ok"},
                ("POST", "/api/admin/live-stats/run"): {
                    "status": "partial",
                    "errors": ["provider timeout"],
                },
                ("GET", "/api/admin/live-stats/status"): {"status": "failed"},
            })

    def test_failed_result_fails_cron(self):
        with self.assertRaisesRegex(RuntimeError, "did not complete successfully"):
            self.run_main({
                ("GET", "/health"): {"status": "ok", "database": "ok"},
                ("GET", "/api/health"): {"status": "ok", "database": "ok"},
                ("POST", "/api/admin/live-stats/run"): {
                    "status": "failed",
                    "errors": ["provider unavailable"],
                },
                ("GET", "/api/admin/live-stats/status"): {"status": "failed"},
            })

    def test_missing_admin_token_fails_before_provider_call(self):
        OPS_LIVE.ADMIN_TOKEN = ""
        with self.assertRaisesRegex(RuntimeError, "CFF_ADMIN_API_TOKEN is required"):
            self.run_main({
                ("GET", "/health"): {"status": "ok", "database": "ok"},
                ("GET", "/api/health"): {"status": "ok", "database": "ok"},
                ("POST", "/api/admin/live-stats/run"): {"status": "succeeded"},
                ("GET", "/api/admin/live-stats/status"): {"status": "ok"},
            })

    def test_worker_result_classifier(self):
        self.assertTrue(OPS_LIVE.successful_worker_result({"status": "succeeded"}))
        self.assertTrue(OPS_LIVE.successful_worker_result({
            "status": "duplicate",
            "code": "duplicate_ingest",
        }))
        self.assertTrue(OPS_LIVE.successful_worker_result({
            "status": "skipped",
            "code": "ingest_already_running",
        }))
        self.assertFalse(OPS_LIVE.successful_worker_result({"status": "partial"}))
        self.assertFalse(OPS_LIVE.successful_worker_result({"status": "failed"}))


if __name__ == "__main__":
    unittest.main()
