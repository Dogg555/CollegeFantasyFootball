#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import os
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock

MODULE_PATH = Path(__file__).with_name("run_espn_roster_once.py")
SPEC = importlib.util.spec_from_file_location("run_espn_roster_once", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class FakeCursor:
    def __init__(self, responses: list[tuple[object, ...]]) -> None:
        self.responses = iter(responses)
        self.executed: list[tuple[str, object]] = []

    def __enter__(self) -> "FakeCursor":
        return self

    def __exit__(self, *_args: object) -> bool:
        return False

    def execute(self, sql: str, params: object = None) -> None:
        self.executed.append((sql, params))

    def fetchone(self) -> tuple[object, ...]:
        return next(self.responses)


class FakeConnection:
    def __init__(self, responses: list[tuple[object, ...]]) -> None:
        self.cursor_instance = FakeCursor(responses)

    def __enter__(self) -> "FakeConnection":
        return self

    def __exit__(self, *_args: object) -> bool:
        return False

    def cursor(self) -> FakeCursor:
        return self.cursor_instance


class OneTimeBootstrapTests(unittest.TestCase):
    def run_case(
        self,
        responses: list[tuple[object, ...]],
    ) -> tuple[int, mock.Mock, FakeConnection]:
        connection = FakeConnection(responses)
        fake_psycopg = types.SimpleNamespace(
            connect=lambda *_args, **_kwargs: connection
        )

        with tempfile.TemporaryDirectory() as directory:
            module_path = Path(directory) / "run_espn_roster_once.py"
            importer_path = Path(directory) / "espn_roster_ingest.py"
            importer_path.write_text("# test importer\n", encoding="utf-8")

            run_mock = mock.Mock()
            environment = {
                "DB_URL": "postgresql://example.invalid/cff",
                "ESPN_ROSTER_AUTO_ONCE": "true",
                "ESPN_ROSTER_SEASON": "2026",
            }
            with (
                mock.patch.dict(os.environ, environment, clear=True),
                mock.patch.dict(sys.modules, {"psycopg": fake_psycopg}),
                mock.patch.object(MODULE, "__file__", str(module_path)),
                mock.patch.object(MODULE.subprocess, "run", run_mock),
            ):
                result = MODULE.main()

        return result, run_mock, connection

    def test_verified_fbs_marker_skips_all_espn_requests(self) -> None:
        result, run_mock, connection = self.run_case(
            [(True,), (True, 4321)]
        )
        self.assertEqual(result, 0)
        run_mock.assert_not_called()
        self.assertEqual(len(connection.cursor_instance.executed), 2)
        self.assertIn(
            "players_espn_fbs",
            connection.cursor_instance.executed[1][0],
        )

    def test_first_run_invokes_importer_and_verifies_rows(self) -> None:
        result, run_mock, connection = self.run_case(
            [(True,), (False, 0), (5678,)]
        )
        self.assertEqual(result, 0)
        run_mock.assert_called_once()
        command = run_mock.call_args.args[0]
        self.assertIn("--season", command)
        self.assertIn("2026", command)
        self.assertNotIn("postgresql://example.invalid/cff", command)
        self.assertEqual(run_mock.call_args.kwargs, {"check": True})
        self.assertIn(
            "raw->>'cffScope' = 'division-i-fbs'",
            connection.cursor_instance.executed[-1][0],
        )
        self.assertEqual(
            connection.cursor_instance.executed[-1][1],
            (2026,),
        )

    def test_zero_committed_rows_fails_after_importer(self) -> None:
        connection = FakeConnection([(True,), (False, 0), (0,)])
        fake_psycopg = types.SimpleNamespace(
            connect=lambda *_args, **_kwargs: connection
        )

        with tempfile.TemporaryDirectory() as directory:
            module_path = Path(directory) / "run_espn_roster_once.py"
            importer_path = Path(directory) / "espn_roster_ingest.py"
            importer_path.write_text("# test importer\n", encoding="utf-8")
            environment = {
                "DB_URL": "postgresql://example.invalid/cff",
                "ESPN_ROSTER_AUTO_ONCE": "true",
                "ESPN_ROSTER_SEASON": "2026",
            }
            with (
                mock.patch.dict(os.environ, environment, clear=True),
                mock.patch.dict(sys.modules, {"psycopg": fake_psycopg}),
                mock.patch.object(MODULE, "__file__", str(module_path)),
                mock.patch.object(MODULE.subprocess, "run"),
            ):
                with self.assertRaises(MODULE.BootstrapError):
                    MODULE.main()

    def test_advisory_lock_contention_skips_import(self) -> None:
        result, run_mock, connection = self.run_case([(False,)])
        self.assertEqual(result, 0)
        run_mock.assert_not_called()
        self.assertEqual(len(connection.cursor_instance.executed), 1)


if __name__ == "__main__":
    unittest.main()
