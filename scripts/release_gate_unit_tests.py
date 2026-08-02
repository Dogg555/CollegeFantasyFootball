#!/usr/bin/env python3
"""Fast unit tests for release-gate parsing and evidence helpers."""
from __future__ import annotations

import unittest
from email.message import EmailMessage

from data_ingestion_validation import evaluate_player_page
from email_acceptance import extract_action_url, plus_alias, token_from_url
from full_lifecycle_tests import derive_standings
from release_gate_common import mask_email, redact_url


class ReleaseGateTests(unittest.TestCase):
    def test_plus_alias(self) -> None:
        self.assertEqual(plus_alias("qa+old@example.com", "cff-123"), "qa+cff-123@example.com")

    def test_email_action_link(self) -> None:
        message = EmailMessage()
        message["Subject"] = "Verify your College Fantasy account"
        message["To"] = "qa+cff@example.com"
        message.set_content("Open https://collegefantasyfootball.com/verify-email.html?token=secret-token")
        url = extract_action_url(message, "/verify-email.html")
        self.assertEqual(token_from_url(url), "secret-token")
        self.assertNotIn("secret-token", redact_url(url))

    def test_player_page_evaluation(self) -> None:
        result = evaluate_player_page(
            [
                {"id": "1", "name": "A Player", "team": "Wyoming", "position": "QB", "conference": "MW", "season": 2026},
                {"id": "2", "name": "B Player", "team": "Utah", "position": "RB", "conference": "Big 12", "season": 2026},
            ],
            2026,
        )
        self.assertEqual(result["duplicateIds"], 0)
        self.assertEqual(result["expectedSeasonRows"], 2)
        self.assertEqual(result["missing"], {})

    def test_derived_standings(self) -> None:
        standings = derive_standings(
            [
                {
                    "status": "final",
                    "homeManager": "a@example.com",
                    "awayManager": "b@example.com",
                    "homeScore": 10,
                    "awayScore": 7,
                }
            ],
            ["a@example.com", "b@example.com", "c@example.com"],
        )
        by_email = {row["managerEmail"]: row for row in standings}
        self.assertEqual(by_email["a@example.com"]["wins"], 1)
        self.assertEqual(by_email["b@example.com"]["losses"], 1)
        self.assertEqual(by_email["c@example.com"]["wins"], 0)

    def test_mask_email(self) -> None:
        masked = mask_email("release-gate@example.com")
        self.assertTrue(masked.endswith("@example.com"))
        self.assertNotIn("release-gate@", masked)


if __name__ == "__main__":
    unittest.main()
