#!/usr/bin/env python3
"""Helpers for ESPN's Division I FBS college-football team directory."""

from __future__ import annotations

import urllib.parse

ESPN_FBS_GROUP_ID = 80
ESPN_TEAM_DIRECTORY_URL = (
    "https://site.web.api.espn.com/apis/site/v2/sports/football/college-football/teams"
)


def fbs_teams_url() -> str:
    """Build the ESPN directory URL scoped to Division I FBS (group 80)."""
    query = urllib.parse.urlencode(
        {
            "groups": ESPN_FBS_GROUP_ID,
            "groupType": "conference",
            "enable": "groups",
        }
    )
    return f"{ESPN_TEAM_DIRECTORY_URL}?{query}"
