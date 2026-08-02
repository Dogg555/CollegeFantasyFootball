#!/usr/bin/env python3
import unittest
import urllib.parse

from espn_roster_ingest import Team, parse_player, parse_teams
from espn_team_directory import ESPN_FBS_GROUP_ID, fbs_teams_url


class EspnRosterIngestTests(unittest.TestCase):
    def test_builds_fbs_team_directory_url(self):
        parsed = urllib.parse.urlparse(fbs_teams_url())
        self.assertEqual(parsed.scheme, "https")
        self.assertEqual(parsed.netloc, "site.web.api.espn.com")
        self.assertEqual(
            parsed.path,
            "/apis/site/v2/sports/football/college-football/teams",
        )
        self.assertEqual(
            urllib.parse.parse_qs(parsed.query),
            {
                "groups": [str(ESPN_FBS_GROUP_ID)],
                "groupType": ["conference"],
                "enable": ["groups"],
            },
        )

    def test_parses_nested_fbs_group_shape_and_conference(self):
        payload = {
            "groups": [
                {
                    "name": "Southeastern Conference",
                    "children": [
                        {
                            "teams": [
                                {
                                    "team": {
                                        "id": "333",
                                        "location": "Alabama",
                                        "displayName": "Alabama Crimson Tide",
                                        "slug": "alabama-crimson-tide",
                                    }
                                }
                            ]
                        }
                    ],
                }
            ]
        }
        self.assertEqual(
            parse_teams(payload),
            [
                Team(
                    "333",
                    "Alabama",
                    "Southeastern Conference",
                    "alabama-crimson-tide",
                )
            ],
        )

    def test_parses_original_team_directory_shape(self):
        payload = {
            "sports": [
                {
                    "leagues": [
                        {
                            "teams": [
                                {
                                    "team": {
                                        "id": "239",
                                        "location": "Baylor",
                                        "displayName": "Baylor Bears",
                                        "slug": "baylor-bears",
                                    }
                                }
                            ]
                        }
                    ]
                }
            ]
        }
        self.assertEqual(
            parse_teams(payload),
            [Team("239", "Baylor", "FBS", "baylor-bears")],
        )

    def test_parses_player_fields_and_preserves_espn_id(self):
        athlete = {
            "id": "4685720",
            "firstName": "Example",
            "lastName": "Player",
            "displayName": "Example Player",
            "position": {"abbreviation": "QB"},
            "experience": {"abbreviation": "SO"},
            "height": 75,
            "displayWeight": "215 lbs",
            "jersey": "7",
        }
        team = Team("333", "Alabama", "SEC")
        player = parse_player(athlete, team, 2026)
        self.assertIsNotNone(player)
        assert player is not None
        self.assertEqual(player.id, "4685720")
        self.assertEqual(player.full_name, "Example Player")
        self.assertEqual(player.position, "QB")
        self.assertEqual(player.year, "SO")
        self.assertEqual(player.height, "6' 3\"")
        self.assertEqual(player.weight, 215)
        self.assertEqual(player.raw["cffSource"], "espn")
        self.assertEqual(player.raw["cffScope"], "division-i-fbs")
        self.assertEqual(player.raw["cffSeason"], 2026)

    def test_rejects_player_without_id(self):
        team = Team("333", "Alabama", "SEC")
        self.assertIsNone(parse_player({"displayName": "No ID"}, team, 2026))


if __name__ == "__main__":
    unittest.main()
