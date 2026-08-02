#!/usr/bin/env python3
"""Helpers for ESPN's conference-filtered college football team directory."""

from __future__ import annotations

import urllib.parse

ESPN_TEAM_DIRECTORY_URL = (
    "https://site.web.api.espn.com/apis/site/v2/sports/football/college-football/teams"
)


def conference_teams_url(group_id: int) -> str:
    """Build the ESPN web API URL that actually applies conference filtering."""
    query = urllib.parse.urlencode(
        {
            "groups": group_id,
            "groupType": "conference",
            "enable": "groups",
        }
    )
    return f"{ESPN_TEAM_DIRECTORY_URL}?{query}"
