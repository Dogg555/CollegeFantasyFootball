#!/usr/bin/env python3
import unittest

from espn_roster_ingest import Team, parse_player, parse_teams


class EspnRosterIngestTests(unittest.TestCase):
    def test_parses_team_directory_shape(self):
        payload = {
            "sports": [
                {
                    "leagues": [
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
                    ]
                }
            ]
        }
        self.assertEqual(
            parse_teams(payload, "SEC"),
            [Team("333", "Alabama", "SEC", "alabama-crimson-tide")],
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
        self.assertEqual(player.raw["cffSeason"], 2026)

    def test_rejects_player_without_id(self):
        team = Team("333", "Alabama", "SEC")
        self.assertIsNone(parse_player({"displayName": "No ID"}, team, 2026))


if __name__ == "__main__":
    unittest.main()
