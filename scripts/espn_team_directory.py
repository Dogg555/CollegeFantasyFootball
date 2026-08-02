#!/usr/bin/env python3
"""Helpers for ESPN's complete college football team directory."""

from __future__ import annotations

import urllib.parse

ESPN_TEAM_DIRECTORY_URL = (
    "https://site.api.espn.com/apis/site/v2/sports/football/college-football/teams"
)


def all_teams_url() -> str:
    """Build the ESPN URL that returns the full college-football team directory."""
    query = urllib.parse.urlencode({"limit": 1000})
    return f"{ESPN_TEAM_DIRECTORY_URL}?{query}"
